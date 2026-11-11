#include "capture_run.h"

#include <bit>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

#ifndef __EMSCRIPTEN__
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>

namespace huxerui::audio::detail {
namespace {

using namespace media;

template<class T, class E>
T Require(Result<T, E> result, CaptureErrorCode code) {
  if (!result.Succeeded()) throw CaptureError{code, std::move(result).Error().message};
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

std::optional<CapturedAudio> TakeBlock(CaptureResult<std::optional<CapturedAudio>> result) {
  if (!result.Succeeded()) throw std::move(result).Error();
  return std::move(result).Value();
}

AudioFormat FirstFormat(const std::optional<CapturedAudio>& block) {
  if (!block) throw CaptureError{CaptureErrorCode::NoAudio, "The recording contains no audio samples."};
  return block->frame.format;
}

MediaTrack MakeTrack(AudioFormat format, CodecId codec, Bytes initialization = {}) {
  return {
      .id = 1,
      .kind = TrackKind::Audio,
      .timescale = format.sample_rate,
      .descriptions = {{
          .codec = codec,
          .initialization_data = std::move(initialization),
          .sample_rate = format.sample_rate,
          .channel_count = format.ChannelCount(),
      }},
  };
}

void WritePacket(MediaMuxer& muxer, const ContainerPacket& packet) {
  if (Require(muxer.TryWrite(packet), CaptureErrorCode::OutputFailed))
    throw CaptureError{CaptureErrorCode::OutputFailed, "The muxer did not accept the audio packet."};
}

void Advance(std::int64_t& samples, std::uint32_t count) {
  if (samples > std::numeric_limits<std::int64_t>::max() - count)
    throw CaptureError{CaptureErrorCode::OutputFailed, "The recording exceeds the sample count limit."};
  samples += count;
}

MediaMuxer OpenRecordingMuxer(RecordingDestination destination, ContainerFormat container, const MediaTrack& track) {
  const auto tracks = std::span(&track, 1);
  return std::visit([container, tracks](auto& output) {
    if constexpr (std::is_same_v<std::decay_t<decltype(output)>, File>) {
      Require(MediaMuxer::ValidateOutput(container, tracks), CaptureErrorCode::OutputFailed);
      auto stream = Require(output.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew),
                            CaptureErrorCode::OutputFailed);
      return Require(MediaMuxer::Open(std::move(stream), container, tracks), CaptureErrorCode::OutputFailed);
    } else {
      return Require(MediaMuxer::Open(std::move(output), container, tracks), CaptureErrorCode::OutputFailed);
    }
  }, destination);
}

void ValidateRecording(const RecordingDestination& destination, CodecId codec, ContainerFormat container) {
  if (codec != CodecId::PcmS16Le && codec != CodecId::Aac)
    throw CaptureError{CaptureErrorCode::Unavailable, "Recording supports S16 little-endian PCM or AAC-LC."};
  const bool pcm_container = container == ContainerFormat::Wave || container == ContainerFormat::Rf64 ||
                             container == ContainerFormat::Mov;
  const bool aac_container = container == ContainerFormat::Adts || container == ContainerFormat::Mp4 ||
                             container == ContainerFormat::Mov;
  if (codec == CodecId::PcmS16Le ? !pcm_container : !aac_container)
    throw CaptureError{CaptureErrorCode::OutputFailed, "The requested codec and container combination is unavailable for recording."};
  if (container != ContainerFormat::Adts && std::holds_alternative<OutputStream>(destination))
    throw CaptureError{CaptureErrorCode::OutputFailed, "The requested recording container requires random writes."};
}

class PcmRecording {
public:
  PcmRecording(AudioFormat format, RecordingDestination destination, ContainerFormat container)
      : format_(format), muxer_(OpenRecordingMuxer(std::move(destination), container,
                                                  MakeTrack(format, CodecId::PcmS16Le))) {
    static_assert(std::endian::native == std::endian::little, "PCM export requires little-endian samples");
  }

  void Append(const AudioFrame& frame) {
    WritePacket(muxer_, {.track_id = 1,
                        .data = frame.planes.front(),
                        .pts = MediaTimestamp{samples_, format_.sample_rate},
                        .duration = frame.Duration()});
    Advance(samples_, frame.sample_count);
  }

  RecordingSummary Finish() {
    Require(muxer_.Finish(), CaptureErrorCode::OutputFailed);
    return {format_, {samples_, format_.sample_rate}};
  }

private:
  AudioFormat format_;
  MediaMuxer muxer_;
  std::int64_t samples_ = 0;
};

class EncoderSignal {
public:
#ifdef __EMSCRIPTEN__
  void Notify() {
    if (auto waiting = std::exchange(waiting_, {})) {
      waiting->Complete();
    } else {
      pending_ = true;
    }
  }

  static Task<void> Wait(std::shared_ptr<EncoderSignal> signal) {
    return AwaitCallback<void>([signal = std::move(signal)](CallbackCompletion<void> completion)
                                  -> std::function<void()> {
      if (std::exchange(signal->pending_, false)) {
        completion.Complete();
        return {};
      }
      std::function<void()> cancel = [signal] { signal->waiting_.reset(); };
      signal->waiting_ = std::move(completion);
      return cancel;
    });
  }

private:
  std::optional<CallbackCompletion<void>> waiting_;
#else
  void Notify() {
    {
      std::lock_guard lock(mutex_);
      pending_ = true;
    }
    changed_.notify_one();
  }

  void Wait(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, stop, [this] { return pending_; });
    pending_ = false;
  }

private:
  std::mutex mutex_;
  std::condition_variable_any changed_;
#endif
  bool pending_ = false;
};

class AacRecording {
public:
  AacRecording(AudioFormat format, RecordingDestination destination, ContainerFormat container, std::uint32_t bitrate)
      : format_(format), destination_(std::move(destination)), container_(container),
        encoder_(Require(AudioEncoder::Create({.input = format, .bitrate = bitrate,
                                               .wakeup = [signal = signal_] { signal->Notify(); }}),
                         CaptureErrorCode::EncodeFailed)) {}

  bool TryAppend(AudioFrame& frame) {
    Drain();
    frame.pts = {samples_, format_.sample_rate};
    if (Require(encoder_.TrySubmit(frame), CaptureErrorCode::EncodeFailed) == SubmitStatus::WouldBlock) return false;
    Advance(samples_, frame.sample_count);
    return true;
  }

  void FinishInput() {
    Require(encoder_.Finish(), CaptureErrorCode::EncodeFailed);
  }

  bool Drain() {
    while (auto event = Require(encoder_.TryRead(), CaptureErrorCode::EncodeFailed)) {
      if (auto* description = std::get_if<AudioEncodeFormat>(&*event)) {
        if (muxer_) throw CaptureError{CaptureErrorCode::EncodeFailed, "The AAC configuration changed."};
        const auto track = MakeTrack(format_, CodecId::Aac, std::move(description->initialization_data));
        muxer_.emplace(OpenRecordingMuxer(std::move(destination_), container_, track));
      } else if (auto* packet = std::get_if<EncodedPacket>(&*event)) {
        if (!muxer_) throw CaptureError{CaptureErrorCode::EncodeFailed, "AAC configuration is missing."};
        ContainerPacket output{.track_id = 1, .data = packet->data, .duration = packet->duration};
        if (container_ != ContainerFormat::Adts) {
          output.pts = packet->pts;
          output.dts = packet->dts;
          output.sync_sample = packet->key_frame;
        }
        WritePacket(*muxer_, output);
      } else if (auto* summary = std::get_if<AudioEncodeSummary>(&*event)) {
        if (muxer_ && container_ != ContainerFormat::Adts) {
          Require(muxer_->EndTrack(1, summary->priming_samples, summary->padding_samples),
                  CaptureErrorCode::OutputFailed);
        }
      } else if (std::holds_alternative<EndOfStream>(*event)) {
        return true;
      }
    }
    return false;
  }

  RecordingSummary Finish() {
    if (!muxer_) throw CaptureError{CaptureErrorCode::EncodeFailed, "The encoder returned no AAC stream."};
    Require(muxer_->Finish(), CaptureErrorCode::OutputFailed);
    return {format_, {samples_, format_.sample_rate}};
  }

#ifdef __EMSCRIPTEN__
  Task<void> Wait() const { return EncoderSignal::Wait(signal_); }
#else
  void Wait(std::stop_token stop) const { signal_->Wait(stop); }
#endif

private:
  AudioFormat format_;
  RecordingDestination destination_;
  ContainerFormat container_;
  std::shared_ptr<EncoderSignal> signal_ = std::make_shared<EncoderSignal>();
  AudioEncoder encoder_;
  std::optional<MediaMuxer> muxer_;
  std::int64_t samples_ = 0;
};

#ifndef __EMSCRIPTEN__
void CheckCanceled(std::stop_token stop) {
  if (stop.stop_requested())
    throw CaptureError{CaptureErrorCode::Interrupted, "Audio export was canceled."};
}

RecordingSummary WriteRecording(AudioReader reader, RecordingDestination destination, CodecId codec,
                                ContainerFormat container, std::uint32_t bitrate, std::stop_token stop) {
  ValidateRecording(destination, codec, container);
  auto block = TakeBlock(reader.Read(stop));
  CheckCanceled(stop);
  const auto format = FirstFormat(block);
  if (codec == CodecId::PcmS16Le) {
    PcmRecording recording(format, std::move(destination), container);
    do {
      recording.Append(block->frame);
    } while ((block = TakeBlock(reader.Read(stop))));
    CheckCanceled(stop);
    return recording.Finish();
  }

  AacRecording recording(format, std::move(destination), container, bitrate);
  do {
    for (;;) {
      CheckCanceled(stop);
      if (recording.TryAppend(block->frame)) break;
      recording.Wait(stop);
    }
  } while ((block = TakeBlock(reader.Read(stop))));

  recording.FinishInput();
  while (!recording.Drain()) {
    recording.Wait(stop);
    CheckCanceled(stop);
  }
  CheckCanceled(stop);
  return recording.Finish();
}
#endif

} // namespace

Task<RecordingResult> RecordAudio(AudioReader reader, RecordingDestination destination, media::CodecId codec,
                                  media::ContainerFormat container, std::uint32_t bitrate) {
#ifdef __EMSCRIPTEN__
  try {
    ValidateRecording(destination, codec, container);
    auto block = TakeBlock(co_await reader.ReadAsync());
    const auto format = FirstFormat(block);
    if (codec == media::CodecId::PcmS16Le) {
      PcmRecording recording(format, std::move(destination), container);
      do {
        recording.Append(block->frame);
      } while ((block = TakeBlock(co_await reader.ReadAsync())));
      co_return RecordingResult::Success(recording.Finish());
    }

    AacRecording recording(format, std::move(destination), container, bitrate);
    do {
      while (!recording.TryAppend(block->frame)) co_await recording.Wait();
    } while ((block = TakeBlock(co_await reader.ReadAsync())));
    recording.FinishInput();
    while (!recording.Drain()) co_await recording.Wait();
    co_return RecordingResult::Success(recording.Finish());
  } catch (CaptureError& error) {
    co_return RecordingResult::Failure(std::move(error));
  } catch (const std::exception& error) {
    co_return RecordingResult::Failure({CaptureErrorCode::OutputFailed, error.what()});
  }
#else
  return AwaitCallback<RecordingResult>(
      [reader = std::move(reader), destination = std::move(destination), codec, container, bitrate](
          CallbackCompletion<RecordingResult> completion) mutable -> std::function<void()> {
        std::stop_source stop;
        std::function<void()> cancel = [stop]() mutable { stop.request_stop(); };
        try {
          std::thread([reader = std::move(reader), destination = std::move(destination), codec, container, bitrate, completion,
                       token = stop.get_token()]() mutable {
            try {
              auto summary = WriteRecording(std::move(reader), std::move(destination), codec, container, bitrate, token);
              completion.Complete(RecordingResult::Success(summary));
            } catch (CaptureError& error) {
              completion.Complete(RecordingResult::Failure(std::move(error)));
            } catch (const std::exception& error) {
              completion.Complete(RecordingResult::Failure({CaptureErrorCode::OutputFailed, error.what()}));
            }
          }).detach();
        } catch (const std::exception& error) {
          completion.Complete(RecordingResult::Failure({CaptureErrorCode::OutputFailed, error.what()}));
        }
        return cancel;
      });
#endif
}

} // namespace huxerui::audio::detail
