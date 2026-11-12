#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <variant>

#include <huxerui/audiocapture.h>

namespace huxerui::audio::detail {

using ReadResult = CaptureResult<std::optional<CapturedAudio>>;
inline constexpr std::size_t max_pcm_blocks = 128;
inline constexpr std::size_t max_pcm_bytes = 512 * 1024;

class CaptureRun : public std::enable_shared_from_this<CaptureRun> {
public:
  explicit CaptureRun(std::function<void(media::AudioFormat)> ready, CaptureOptions options = {});

  bool Publish(media::AudioFormat format, CaptureClock clock, media::MediaTimestamp pts,
               std::span<const std::int16_t> pcm);
  void End(std::optional<CaptureError> error = {});
  std::optional<CaptureError> Error() const;
  std::stop_token StopToken() const { return stop_.get_token(); }
  const CaptureOptions& Options() const { return options_; }
  media::AudioFormat RequestedFormat(media::AudioFormat format) const;
  bool WaitForFirstFrame(std::chrono::milliseconds timeout);
  void WaitForStop();
  bool SetInputLevelMonitoring(bool enabled);
  std::optional<AudioInputLevel> TakeInputLevel();

  CaptureResult<AudioReader> OpenReader(std::function<void()> recording_released = {});
  void ReleaseReader(std::uint64_t reader_id);
  CaptureResult<void> FinishInput(std::uint64_t reader_id, std::optional<media::MediaTimestamp> capture_cutoff);
  std::optional<CaptureError> ReaderError(std::uint64_t reader_id) const;
  void SetRecordingPaused(bool paused);
  bool RecordingPaused() const;
  void StopRecording();
  ReadResult Read(std::uint64_t reader_id, std::stop_token cancellation);
  Task<ReadResult> ReadAsync(std::uint64_t reader_id);

private:
  // Represent time as whole seconds plus a whole and fractional frame offset.
  struct PcmPosition {
    std::int64_t seconds;
    std::uint64_t frames;
    media::MediaTimestamp fraction;

    static PcmPosition FromTimestamp(media::MediaTimestamp time, std::uint32_t rate);
    PcmPosition Advance(std::uint32_t count, std::uint32_t rate) const;
    int Compare(const PcmPosition& other) const;
    std::uint32_t FramesUntil(const PcmPosition& cutoff, std::uint32_t rate, std::uint32_t count) const;
  };

  struct InputLevelWindow {
    std::array<std::int32_t, 2> peaks{};
    std::array<double, 2> square_sums{};
    std::uint64_t frames = 0;

    void Add(std::span<const std::int16_t> pcm, std::uint32_t channels);
  };

  void SealLocked(std::optional<CaptureError> error);
  void FinishInputLocked();
  void TrimQueueLocked(const PcmPosition& cutoff);
  bool ReadReadyLocked() const;
  ReadResult TakeReadLocked();
  void CompleteRead();

  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::stop_source stop_;
  const CaptureOptions options_;
  std::function<void(media::AudioFormat)> ready_;
  std::optional<media::AudioFormat> format_;
  std::optional<CaptureClock> clock_;
  std::optional<media::MediaTimestamp> last_pts_;
  std::optional<PcmPosition> capture_end_;
  std::optional<CaptureError> error_;
  std::optional<InputLevelWindow> input_level_;
  std::shared_ptr<CallbackCompletion<ReadResult>> waiting_read_;
  std::function<void()> recording_released_;
  std::optional<media::MediaTimestamp> reader_cutoff_;
  std::optional<PcmPosition> dequeued_end_;
  std::deque<CapturedAudio> queue_;
  std::size_t queued_bytes_ = 0;
  std::uint64_t reader_id_ = 0;
  bool ended_ = false;
  bool attached_ = false;
  bool recording_paused_ = false;
  bool reader_input_finished_ = false;
};

using RecordingResult = CaptureResult<RecordingSummary>;
using RecordingDestination = std::variant<File, RandomAccessStream, OutputStream>;

Task<RecordingResult> RecordAudio(AudioReader reader, RecordingDestination destination, media::CodecId codec,
                                  media::ContainerFormat container, std::uint32_t bitrate);

inline constexpr char capture_module[] = "audio/Capture";
using CaptureCompleted = std::function<void(std::optional<CaptureError>)>;
using InputDevicesResult = CaptureResult<std::vector<InputDevice>>;

struct CaptureBackend {
  std::function<void()> (*start)(const std::shared_ptr<CaptureRun>&, CaptureCompleted) = nullptr;
  bool use_sdk_permission = true;
  std::function<Task<InputDevicesResult>()> enumerate;
};

/// Blocks on the producer thread until capture stops or fails. Releases the capture device before returning.
std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run);

/// Queries platform input devices on a worker.
InputDevicesResult EnumeratePlatformInputDevices();

/// Creates an input query retaining the Android application context for worker calls.
std::function<Task<InputDevicesResult>()> MakeAndroidInputEnumerator(Runtime& runtime);

/// Starts browser capture. The returned function retires callbacks and releases the input track.
std::function<void()> StartWebCapture(const std::shared_ptr<CaptureRun>& run, CaptureCompleted completed);

/// Queries browser input devices on the browser event loop.
Task<InputDevicesResult> EnumerateWebInputDevices();

/// Maps a sample position to a clock using a position and timestamp correlation.
std::optional<media::MediaTimestamp> SampleTimestamp(std::int64_t first_frame, std::int64_t anchor_frame,
                                                     std::int64_t anchor_ns, std::uint32_t sample_rate);

inline constexpr media::AudioFormat mono_48k{
    48000, media::AudioSampleFormat::S16, media::AudioPacking::Interleaved, media::AudioChannelLayout::Mono};

} // namespace huxerui::audio::detail
