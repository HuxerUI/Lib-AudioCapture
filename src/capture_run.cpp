#include "capture_run.h"

#include <limits>
#include <utility>
#include <vector>

namespace huxerui::audio::detail {
namespace {

constexpr std::size_t max_blocks = 128;
constexpr std::size_t max_bytes = 512 * 1024;

} // namespace

CaptureRun::CaptureRun(std::function<void(media::AudioFormat)> ready) : ready_(std::move(ready)) {}

bool CaptureRun::Publish(media::AudioFormat format, CaptureClock clock, media::MediaTimestamp pts,
                         std::span<const std::int16_t> pcm) {
  const auto channels = format.ChannelCount();
  if (!channels || !format.sample_rate || format.sample_format != media::AudioSampleFormat::S16 ||
      format.packing != media::AudioPacking::Interleaved || !pts.timescale ||
      pcm.empty() || pcm.size() % channels || pcm.size() / channels > std::numeric_limits<std::uint32_t>::max()) {
    End(CaptureError{CaptureErrorCode::CaptureFailed, "Invalid PCM or microphone timestamp."});
    return false;
  }

  std::function<void(media::AudioFormat)> ready;
  std::optional<CaptureError> error;
  {
    std::lock_guard lock(mutex_);
    if (ended_) return false;
    if (format_ && (*format_ != format || clock_ != clock)) {
      error = CaptureError{CaptureErrorCode::Discontinuity, "The microphone format or clock changed."};
    } else if (last_pts_ && pts.Compare(*last_pts_) != 1) {
      error = CaptureError{CaptureErrorCode::Discontinuity, "The microphone timestamp did not advance."};
    } else if (attached_ && (queue_.size() == max_blocks || pcm.size_bytes() > max_bytes - queued_bytes_)) {
      error = CaptureError{CaptureErrorCode::QueueOverflow, "The audio reader could not keep up with capture."};
    } else {
      if (attached_) {
        auto storage = std::make_shared<const std::vector<std::int16_t>>(pcm.begin(), pcm.end());
        queue_.push_back({
            .frame = {
                .format = format,
                .pts = pts,
                .sample_count = static_cast<std::uint32_t>(pcm.size() / channels),
                .planes = {BufferReference(std::as_bytes(std::span(*storage)), storage)},
            },
            .clock = clock,
        });
        queued_bytes_ += pcm.size_bytes();
      }
      format_ = format;
      clock_ = clock;
      last_pts_ = pts;
      ready = std::exchange(ready_, {});
    }
    if (error) SealLocked(std::move(error));
  }
  if (error) {
    stop_.request_stop();
    changed_.notify_all();
    CompleteRead();
    return false;
  }
  changed_.notify_all();
  CompleteRead();
  if (ready) ready(format);
  return true;
}

void CaptureRun::End(std::optional<CaptureError> error) {
  {
    std::lock_guard lock(mutex_);
    if (ended_) return;
    SealLocked(std::move(error));
  }
  stop_.request_stop();
  changed_.notify_all();
  CompleteRead();
}

void CaptureRun::SealLocked(std::optional<CaptureError> error) {
  ended_ = true;
  error_ = std::move(error);
  ready_ = {};
  if (error_) {
    queue_.clear();
    queued_bytes_ = 0;
  }
}

std::optional<CaptureError> CaptureRun::Error() const {
  std::lock_guard lock(mutex_);
  return error_;
}

bool CaptureRun::WaitForFirstFrame(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  return changed_.wait_for(lock, timeout, [this] { return format_ || ended_; });
}

void CaptureRun::WaitForStop() {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [this] { return ended_; });
}

CaptureResult<AudioReader> CaptureRun::OpenReader() {
  std::lock_guard lock(mutex_);
  if (ended_)
    return CaptureResult<AudioReader>::Failure({CaptureErrorCode::NotReady, "This capture run has ended."});
  if (attached_)
    return CaptureResult<AudioReader>::Failure({CaptureErrorCode::ReaderInUse, "This run already has an audio reader."});
  auto reader = AudioReader(shared_from_this());
  attached_ = true;
  ++reader_id_;
  return CaptureResult<AudioReader>::Success(std::move(reader));
}

void CaptureRun::ReleaseReader() {
  std::shared_ptr<CallbackCompletion<ReadResult>> completion;
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    queued_bytes_ = 0;
    attached_ = false;
    completion = std::exchange(waiting_read_, {});
  }
  if (completion) {
    completion->Complete(ReadResult::Failure({CaptureErrorCode::Interrupted, "The audio reader was released."}));
  }
}

ReadResult CaptureRun::Read(std::stop_token cancellation) {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, cancellation, [this] { return ended_ || !queue_.empty(); });
  if (cancellation.stop_requested())
    return ReadResult::Failure({CaptureErrorCode::Interrupted, "Audio reading was canceled."});
  return TakeReadLocked();
}

ReadResult CaptureRun::TakeReadLocked() {
  if (error_) return ReadResult::Failure(*error_);
  if (queue_.empty()) return ReadResult::Success(std::nullopt);

  auto value = std::move(queue_.front());
  queued_bytes_ -= value.frame.planes.front().AsBytes().size();
  queue_.pop_front();
  return ReadResult::Success(std::move(value));
}

void CaptureRun::CompleteRead() {
  std::unique_lock lock(mutex_);
  if (!waiting_read_ || (!ended_ && queue_.empty())) return;
  auto result = TakeReadLocked();
  auto completion = std::exchange(waiting_read_, {});
  lock.unlock();
  completion->Complete(std::move(result));
}

Task<ReadResult> CaptureRun::ReadAsync() {
  std::lock_guard lock(mutex_);
  return AwaitCallback<ReadResult>(
      [run = shared_from_this(), reader_id = reader_id_](CallbackCompletion<ReadResult> completion)
          -> std::function<void()> {
        auto pending = std::make_shared<CallbackCompletion<ReadResult>>(std::move(completion));
        std::function<void()> cancel = [run, pending] {
          std::lock_guard lock(run->mutex_);
          if (run->waiting_read_ == pending) run->waiting_read_.reset();
        };
        std::unique_lock lock(run->mutex_);
        if (!run->attached_ || run->reader_id_ != reader_id) {
          lock.unlock();
          pending->Complete(ReadResult::Failure({CaptureErrorCode::NotReady, "The audio reader was released."}));
          return {};
        }
        if (run->waiting_read_) {
          lock.unlock();
          pending->Complete(ReadResult::Failure({CaptureErrorCode::ReaderInUse, "An audio read is already pending."}));
          return {};
        }
        run->waiting_read_ = pending;
        lock.unlock();
        run->CompleteRead();
        return cancel;
      });
}

std::optional<media::MediaTimestamp> SampleTimestamp(std::int64_t first_frame, std::int64_t anchor_frame,
                                                     std::int64_t anchor_ns, std::uint32_t sample_rate) {
  if (first_frame < 0 || anchor_frame < 0 || anchor_ns < 0) return {};
  const auto offset = media::MediaTimestamp{first_frame - anchor_frame, sample_rate}.Rescale(1000000000);
  if (!offset || offset->ticks < -anchor_ns || offset->ticks > std::numeric_limits<std::int64_t>::max() - anchor_ns)
    return {};
  return media::MediaTimestamp{anchor_ns + offset->ticks, 1000000000};
}

} // namespace huxerui::audio::detail

namespace huxerui::audio {

AudioReader::AudioReader(std::shared_ptr<detail::CaptureRun> run) : run_(std::move(run)) {}
AudioReader::AudioReader(AudioReader&&) noexcept = default;

AudioReader& AudioReader::operator=(AudioReader&& other) noexcept {
  if (this != &other) {
    if (run_) run_->ReleaseReader();
    run_ = std::move(other.run_);
  }
  return *this;
}

AudioReader::~AudioReader() {
  if (run_) run_->ReleaseReader();
}

CaptureResult<std::optional<CapturedAudio>> AudioReader::Read(std::stop_token cancellation) {
  if (!run_) return detail::ReadResult::Failure({CaptureErrorCode::NotReady, "The audio reader has been moved."});
#ifdef __EMSCRIPTEN__
  static_cast<void>(cancellation);
  return detail::ReadResult::Failure({CaptureErrorCode::Unavailable, "Use ReadAsync to read microphone PCM on Web."});
#else
  return run_->Read(cancellation);
#endif
}

Task<CaptureResult<std::optional<CapturedAudio>>> AudioReader::ReadAsync() {
  if (run_) return run_->ReadAsync();
  return AwaitCallback<detail::ReadResult>([](CallbackCompletion<detail::ReadResult> completion) {
    completion.Complete(detail::ReadResult::Failure({CaptureErrorCode::NotReady, "The audio reader has been moved."}));
    return std::function<void()>{};
  });
}

} // namespace huxerui::audio
