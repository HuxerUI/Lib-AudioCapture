#include <huxerui/audiocapture.h>

#include <bit>
#include <limits>
#include <type_traits>
#include <utility>

#ifndef __EMSCRIPTEN__
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>

namespace huxerui::audio {
namespace {

using namespace media;
using RecordingResult = CaptureResult<RecordingSummary>;

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

RandomAccessStream OpenWaveDestination(File destination) {
  return Require(destination.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew),
                 CaptureErrorCode::OutputFailed);
}

RandomAccessStream OpenWaveDestination(RandomAccessStream destination) { return destination; }

template<class Destination>
MediaMuxer OpenWave(AudioFormat format, Destination destination) {
  static_assert(std::endian::native == std::endian::little, "WAVE export requires little-endian PCM");
  const auto track = MakeTrack(format, CodecId::PcmS16Le);
  const auto tracks = std::span(&track, 1);
  Require(MediaMuxer::ValidateOutput(ContainerFormat::Wave, tracks), CaptureErrorCode::OutputFailed);
  return Require(MediaMuxer::Open(OpenWaveDestination(std::move(destination)), ContainerFormat::Wave, tracks),
                 CaptureErrorCode::OutputFailed);
}

class WaveRecording {
public:
  template<class Destination>
  WaveRecording(AudioFormat format, Destination destination)
      : format_(format), muxer_(OpenWave(format, std::move(destination))) {}

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
  AacRecording(AudioFormat format, OutputStream destination, std::uint32_t bitrate)
      : format_(format), destination_(std::move(destination)),
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
        muxer_.emplace(Require(
            MediaMuxer::Open(std::move(destination_), ContainerFormat::Adts, std::span(&track, 1)),
            CaptureErrorCode::OutputFailed));
      } else if (auto* packet = std::get_if<EncodedPacket>(&*event)) {
        if (!muxer_) throw CaptureError{CaptureErrorCode::EncodeFailed, "AAC configuration is missing."};
        // ADTS carries every access unit in order, including encoder preroll and final padding.
        WritePacket(*muxer_, {.track_id = 1, .data = packet->data, .duration = packet->duration});
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
  OutputStream destination_;
  std::shared_ptr<EncoderSignal> signal_ = std::make_shared<EncoderSignal>();
  AudioEncoder encoder_;
  std::optional<MediaMuxer> muxer_;
  std::int64_t samples_ = 0;
};

#ifdef __EMSCRIPTEN__
template<class Destination>
Task<RecordingResult> WriteWave(AudioReader reader, Destination destination) {
  try {
    auto block = TakeBlock(co_await reader.ReadAsync());
    WaveRecording recording(FirstFormat(block), std::move(destination));
    do {
      recording.Append(block->frame);
    } while ((block = TakeBlock(co_await reader.ReadAsync())));
    co_return RecordingResult::Success(recording.Finish());
  } catch (CaptureError& error) {
    co_return RecordingResult::Failure(std::move(error));
  } catch (const std::exception& error) {
    co_return RecordingResult::Failure({CaptureErrorCode::OutputFailed, error.what()});
  }
}

Task<RecordingResult> WriteAac(AudioReader reader, OutputStream destination, std::uint32_t bitrate) {
  try {
    auto block = TakeBlock(co_await reader.ReadAsync());
    AacRecording recording(FirstFormat(block), std::move(destination), bitrate);
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
}
#else
void CheckCanceled(std::stop_token stop) {
  if (stop.stop_requested())
    throw CaptureError{CaptureErrorCode::Interrupted, "Audio export was canceled."};
}

template<class Destination>
RecordingSummary WriteWave(AudioReader reader, Destination destination, std::stop_token stop) {
  auto block = TakeBlock(reader.Read(stop));
  CheckCanceled(stop);
  WaveRecording recording(FirstFormat(block), std::move(destination));
  do {
    recording.Append(block->frame);
  } while ((block = TakeBlock(reader.Read(stop))));
  CheckCanceled(stop);
  return recording.Finish();
}

RecordingSummary WriteAac(AudioReader reader, OutputStream destination, std::uint32_t bitrate, std::stop_token stop) {
  auto block = TakeBlock(reader.Read(stop));
  AacRecording recording(FirstFormat(block), std::move(destination), bitrate);
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

template<class Destination, class Write>
Task<RecordingResult> StartRecording(AudioReader reader, Destination destination, Write write) {
  return AwaitCallback<RecordingResult>(
      [reader = std::move(reader), destination = std::move(destination), write](
          CallbackCompletion<RecordingResult> completion) mutable -> std::function<void()> {
        std::stop_source stop;
        std::function<void()> cancel = [stop]() mutable { stop.request_stop(); };
        try {
          std::thread([reader = std::move(reader), destination = std::move(destination), write, completion,
                       token = stop.get_token()]() mutable {
            try {
              auto summary = write(std::move(reader), std::move(destination), token);
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
}
#endif

} // namespace

Task<CaptureResult<RecordingSummary>> RecordWaveAsync(AudioReader reader, File destination) {
#ifdef __EMSCRIPTEN__
  return WriteWave(std::move(reader), std::move(destination));
#else
  return StartRecording(std::move(reader), std::move(destination), WriteWave<File>);
#endif
}

Task<CaptureResult<RecordingSummary>> RecordWaveAsync(AudioReader reader, RandomAccessStream destination) {
#ifdef __EMSCRIPTEN__
  return WriteWave(std::move(reader), std::move(destination));
#else
  return StartRecording(std::move(reader), std::move(destination), WriteWave<RandomAccessStream>);
#endif
}

Task<CaptureResult<RecordingSummary>> RecordAacAsync(AudioReader reader, OutputStream destination,
                                                     std::uint32_t bitrate) {
#ifdef __EMSCRIPTEN__
  return WriteAac(std::move(reader), std::move(destination), bitrate);
#else
  return StartRecording(std::move(reader), std::move(destination),
                        [bitrate](AudioReader input, OutputStream output, std::stop_token stop) {
                          return WriteAac(std::move(input), std::move(output), bitrate, stop);
                        });
#endif
}

} // namespace huxerui::audio
