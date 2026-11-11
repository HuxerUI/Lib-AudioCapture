#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>

#include <huxerui/audiocapture.h>

namespace huxerui::audio::detail {

using ReadResult = CaptureResult<std::optional<CapturedAudio>>;

class CaptureRun : public std::enable_shared_from_this<CaptureRun> {
public:
  explicit CaptureRun(std::function<void(media::AudioFormat)> ready);

  bool Publish(media::AudioFormat format, CaptureClock clock, media::MediaTimestamp pts,
               std::span<const std::int16_t> pcm);
  void End(std::optional<CaptureError> error = {});
  std::optional<CaptureError> Error() const;
  std::stop_token StopToken() const { return stop_.get_token(); }
  bool WaitForFirstFrame(std::chrono::milliseconds timeout);
  void WaitForStop();

  CaptureResult<AudioReader> OpenReader();
  void ReleaseReader();
  ReadResult Read(std::stop_token cancellation);
  Task<ReadResult> ReadAsync();

private:
  void SealLocked(std::optional<CaptureError> error);
  ReadResult TakeReadLocked();
  void CompleteRead();

  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::stop_source stop_;
  std::function<void(media::AudioFormat)> ready_;
  std::optional<media::AudioFormat> format_;
  std::optional<CaptureClock> clock_;
  std::optional<media::MediaTimestamp> last_pts_;
  std::optional<CaptureError> error_;
  std::shared_ptr<CallbackCompletion<ReadResult>> waiting_read_;
  std::deque<CapturedAudio> queue_;
  std::size_t queued_bytes_ = 0;
  std::uint64_t reader_id_ = 0;
  bool ended_ = false;
  bool attached_ = false;
};

inline constexpr char capture_module[] = "audio/Capture";
using CaptureCompleted = std::function<void(std::optional<CaptureError>)>;

struct CaptureBackend {
  std::function<void()> (*start)(const std::shared_ptr<CaptureRun>&, CaptureCompleted) = nullptr;
  bool use_sdk_permission = true;
};

/// Blocks on the producer thread until capture stops or fails. Releases the capture device before returning.
std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run);

/// Starts browser capture. The returned function retires callbacks and releases the input track.
std::function<void()> StartWebCapture(const std::shared_ptr<CaptureRun>& run, CaptureCompleted completed);

/// Maps a sample position to a clock using a position and timestamp correlation.
std::optional<media::MediaTimestamp> SampleTimestamp(std::int64_t first_frame, std::int64_t anchor_frame,
                                                     std::int64_t anchor_ns, std::uint32_t sample_rate);

inline constexpr media::AudioFormat mono_48k{
    48000, media::AudioSampleFormat::S16, media::AudioPacking::Interleaved, media::AudioChannelLayout::Mono};

} // namespace huxerui::audio::detail
