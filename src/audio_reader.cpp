#include "capture_run.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>

namespace huxerui::audio::detail {
namespace {

template<class T>
T Take(CaptureResult<T> result) {
  if (!result.Succeeded()) throw std::move(result).Error();
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

media::MediaTimestamp OffsetTimestamp(media::MediaTimestamp pts, std::uint32_t frames, std::uint32_t rate) {
  if (!frames) return pts;
  const auto at_scale = [=](std::uint32_t scale) -> std::optional<media::MediaTimestamp> {
    const auto start = pts.Rescale(scale);
    const auto offset = media::MediaTimestamp{frames, rate}.Rescale(scale);
    if (!start || !offset || start->ticks > std::numeric_limits<std::int64_t>::max() - offset->ticks) return {};
    return media::MediaTimestamp{start->ticks + offset->ticks, scale};
  };
  const auto exact_scale = std::lcm<std::uint64_t>(pts.timescale, rate);
  if (exact_scale <= std::numeric_limits<std::uint32_t>::max()) {
    if (auto exact = at_scale(static_cast<std::uint32_t>(exact_scale))) return *exact;
  }
  if (auto rounded = at_scale(std::max(pts.timescale, rate))) return *rounded;
  throw CaptureError{CaptureErrorCode::CaptureFailed, "The processed PCM capture timestamp overflowed."};
}

class ReadGuard {
public:
  explicit ReadGuard(std::atomic_flag& flag) : flag_(flag), acquired_(!flag.test_and_set()) {}
  ReadGuard(const ReadGuard&) = delete;
  ReadGuard& operator=(const ReadGuard&) = delete;
  ~ReadGuard() { if (acquired_) flag_.clear(); }

  bool Acquired() const { return acquired_; }

private:
  std::atomic_flag& flag_;
  bool acquired_;
};

} // namespace

class ProcessedReader {
public:
  ProcessedReader(std::shared_ptr<CaptureRun> run, std::uint64_t reader_id)
      : run_(std::move(run)), reader_id_(reader_id) {}

  void Release() { released_ = true; }

#ifndef __EMSCRIPTEN__
  ReadResult Read(std::stop_token cancellation) {
    const ReadGuard guard(reading_);
    if (!guard.Acquired())
      return ReadResult::Failure({CaptureErrorCode::ReaderInUse, "An audio read is already pending."});

    while (!cancellation.stop_requested()) {
      const bool ready = Prepare(cancellation);
      if (cancellation.stop_requested()) break;
      if (ready) return TakePrepared();
      auto input = run_->Read(reader_id_, cancellation);
      if (input.Succeeded() || !cancellation.stop_requested()) StageInput(std::move(input));
    }
    return ReadResult::Failure({CaptureErrorCode::Interrupted, "Audio reading was canceled."});
  }
#endif

  static Task<ReadResult> ReadAsync(std::shared_ptr<ProcessedReader> reader) {
    if (reader->released_)
      co_return ReadResult::Failure({CaptureErrorCode::NotReady, "The audio reader was released."});
    const ReadGuard guard(reader->reading_);
    if (!guard.Acquired())
      co_return ReadResult::Failure({CaptureErrorCode::ReaderInUse, "An audio read is already pending."});

    for (;;) {
#ifdef __EMSCRIPTEN__
      const bool ready = reader->Prepare();
#else
      const bool ready = co_await reader->workers_.Run(
          [](std::stop_token cancellation, std::shared_ptr<ProcessedReader> reader) {
            return reader->Prepare(cancellation);
          }, reader);
#endif
      if (ready) co_return reader->TakePrepared();
      reader->StageInput(co_await reader->run_->ReadAsync(reader->reader_id_));
    }
  }

private:
  struct SourceBlock {
    media::MediaTimestamp pts;
    std::uint32_t frames;
    std::uint32_t offset = 0;
  };

  void StageInput(ReadResult input) {
    std::lock_guard lock(mutex_);
    pending_input_ = std::move(input);
  }

  bool Prepare(std::stop_token cancellation = {}) {
    std::lock_guard lock(mutex_);
    if (cancellation.stop_requested()) return false;
    if (released_ || prepared_) return true;
    try {
      if (auto error = run_->Error()) throw std::move(*error);
      if (auto input = std::exchange(pending_input_, std::nullopt)) Submit(Take(std::move(*input)));

      if (output_ || (processor_ && ReadOutput())) {
        prepared_ = ReadResult::Success(NextBlock());
      } else if (input_finished_) {
        if (pending_frames_)
          throw CaptureError{CaptureErrorCode::CaptureFailed, "The processor omitted capture-timed PCM frames."};
        prepared_ = ReadResult::Success(std::nullopt);
      }
    } catch (CaptureError& error) {
      Fail(std::move(error));
    } catch (const std::exception& error) {
      Fail({CaptureErrorCode::CaptureFailed, error.what()});
    } catch (...) {
      Fail({CaptureErrorCode::CaptureFailed, "The PCM processor failed."});
    }
    return prepared_.has_value();
  }

  ReadResult TakePrepared() {
    std::lock_guard lock(mutex_);
    if (released_)
      return ReadResult::Failure({CaptureErrorCode::Interrupted, "The audio reader was released."});
    if (prepared_->Succeeded()) {
      if (auto error = run_->Error()) Fail(std::move(*error));
    }
    if (!prepared_->Succeeded() || !prepared_->Value()) return *prepared_;
    auto result = std::move(*prepared_);
    prepared_.reset();
    return result;
  }

  void Submit(const std::optional<CapturedAudio>& input) {
    if (!input) {
      input_finished_ = true;
      if (processor_) Take(processor_->Finish());
      return;
    }

    const auto& frame = input->frame;
    if (!processor_) {
      format_ = frame.format;
      channels_ = format_.ChannelCount();
      capture_clock_ = input->clock;
      processor_ = (*run_->Options().pcm_processor)(format_);
      if (!processor_)
        throw CaptureError{CaptureErrorCode::CaptureFailed, "The PCM processor factory returned no processor."};
      timing_ = processor_->Timing();
      if (timing_ != PcmTiming::CaptureTime && timing_ != PcmTiming::StreamTime)
        throw CaptureError{CaptureErrorCode::CaptureFailed, "The PCM processor returned an invalid timing mode."};
    }

    if (timing_ == PcmTiming::CaptureTime) {
      const auto max_frames = max_pcm_bytes / (channels_ * sizeof(std::int16_t));
      if (sources_.size() == max_pcm_blocks || frame.sample_count > max_frames - pending_frames_)
        throw CaptureError{CaptureErrorCode::QueueOverflow, "The processor buffered too many capture-timed frames."};
      sources_.push_back({frame.pts, frame.sample_count});
      pending_frames_ += frame.sample_count;
    }

    const auto pcm = frame.planes.front().AsBytes();
    Take(processor_->Process({reinterpret_cast<const std::int16_t*>(pcm.data()), pcm.size() / sizeof(std::int16_t)}));
  }

  bool ReadOutput() {
    auto samples = Take(processor_->TryRead());
    if (!samples) return false;
    if (samples->empty() || samples->size() % channels_)
      throw CaptureError{CaptureErrorCode::CaptureFailed, "The PCM processor returned an invalid block."};
    if (samples->size() > max_pcm_bytes / sizeof(std::int16_t))
      throw CaptureError{CaptureErrorCode::QueueOverflow, "The processed PCM block exceeded the byte limit."};
    if (timing_ == PcmTiming::CaptureTime && samples->size() / channels_ > pending_frames_)
      throw CaptureError{CaptureErrorCode::CaptureFailed, "The processor produced frames without capture timestamps."};
    output_ = std::make_shared<const PcmBlock>(std::move(*samples));
    output_offset_ = 0;
    return true;
  }

  CapturedAudio NextBlock() {
    const auto total_frames = static_cast<std::uint32_t>(output_->size() / channels_);
    auto frames = total_frames - output_offset_;
    media::MediaTimestamp pts{stream_frames_, format_.sample_rate};
    std::optional<CaptureClock> clock;
    if (timing_ == PcmTiming::CaptureTime) {
      auto& source = sources_.front();
      frames = std::min(frames, source.frames - source.offset);
      pts = OffsetTimestamp(source.pts, source.offset, format_.sample_rate);
      clock = capture_clock_;
      source.offset += frames;
      pending_frames_ -= frames;
      if (source.offset == source.frames) sources_.pop_front();
    } else {
      if (stream_frames_ > std::numeric_limits<std::int64_t>::max() - frames)
        throw CaptureError{CaptureErrorCode::CaptureFailed, "The processed PCM output timeline overflowed."};
      stream_frames_ += frames;
    }

    const auto pcm = std::span(*output_).subspan(output_offset_ * channels_, frames * channels_);
    CapturedAudio block{
        .frame = {
            .format = format_,
            .pts = pts,
            .sample_count = frames,
            .planes = {BufferReference(std::as_bytes(pcm), output_)},
        },
        .clock = clock,
    };
    output_offset_ += frames;
    if (output_offset_ == total_frames) output_.reset();
    return block;
  }

  void Fail(CaptureError error) {
    prepared_ = ReadResult::Failure(std::move(error));
    processor_.reset();
    output_.reset();
    sources_.clear();
    pending_input_.reset();
    run_->ReleaseReader(reader_id_);
  }

  const std::shared_ptr<CaptureRun> run_;
  const std::uint64_t reader_id_;
  std::atomic_bool released_ = false;
  std::atomic_flag reading_;
#ifndef __EMSCRIPTEN__
  WorkerSequence workers_;
#endif
  std::mutex mutex_;
  std::unique_ptr<PcmProcessor> processor_;
  media::AudioFormat format_;
  std::uint32_t channels_ = 0;
  PcmTiming timing_ = PcmTiming::CaptureTime;
  std::optional<CaptureClock> capture_clock_;
  std::deque<SourceBlock> sources_;
  std::size_t pending_frames_ = 0;
  std::shared_ptr<const PcmBlock> output_;
  std::uint32_t output_offset_ = 0;
  std::int64_t stream_frames_ = 0;
  std::optional<ReadResult> pending_input_;
  std::optional<ReadResult> prepared_;
  bool input_finished_ = false;
};

} // namespace huxerui::audio::detail

namespace huxerui::audio {

AudioReader::AudioReader(std::shared_ptr<detail::CaptureRun> run, std::uint64_t reader_id)
    : run_(std::move(run)), reader_id_(reader_id) {
  if (run_->Options().pcm_processor) processing_ = std::make_shared<detail::ProcessedReader>(run_, reader_id_);
}

AudioReader::AudioReader(AudioReader&&) noexcept = default;

AudioReader& AudioReader::operator=(AudioReader&& other) noexcept {
  if (this != &other) {
    Release();
    run_ = std::move(other.run_);
    processing_ = std::move(other.processing_);
    reader_id_ = other.reader_id_;
  }
  return *this;
}

AudioReader::~AudioReader() { Release(); }

void AudioReader::Release() {
  if (processing_) processing_->Release();
  if (run_) run_->ReleaseReader(reader_id_);
}

CaptureResult<std::optional<CapturedAudio>> AudioReader::Read(std::stop_token cancellation) {
  if (!run_) return detail::ReadResult::Failure({CaptureErrorCode::NotReady, "The audio reader has been moved."});
#ifdef __EMSCRIPTEN__
  static_cast<void>(cancellation);
  return detail::ReadResult::Failure({CaptureErrorCode::Unavailable, "Use ReadAsync to read microphone PCM on Web."});
#else
  if (processing_) return processing_->Read(cancellation);
  return run_->Read(reader_id_, cancellation);
#endif
}

Task<CaptureResult<std::optional<CapturedAudio>>> AudioReader::ReadAsync() {
  if (processing_) return detail::ProcessedReader::ReadAsync(processing_);
  if (run_) return run_->ReadAsync(reader_id_);
  return AwaitCallback<detail::ReadResult>([](CallbackCompletion<detail::ReadResult> completion) {
    completion.Complete(detail::ReadResult::Failure({CaptureErrorCode::NotReady, "The audio reader has been moved."}));
    return std::function<void()>{};
  });
}

} // namespace huxerui::audio
