#include "capture_run.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace huxerui::audio::detail {
namespace {

constexpr double pcm_full_scale = 32768.0;

} // namespace

CaptureRun::CaptureRun(std::function<void(media::AudioFormat)> ready, CaptureOptions options)
    : options_(std::move(options)), ready_(std::move(ready)) {}

media::AudioFormat CaptureRun::RequestedFormat(media::AudioFormat format) const {
  format.sample_rate = options_.sample_rate.value_or(format.sample_rate);
  format.channel_layout = options_.channel_layout.value_or(format.channel_layout);
  return format;
}

void CaptureRun::InputLevelWindow::Add(std::span<const std::int16_t> pcm, std::uint32_t channels) {
  frames += pcm.size() / channels;
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    for (std::size_t index = channel; index < pcm.size(); index += channels) {
      const auto sample = static_cast<std::int32_t>(pcm[index]);
      peaks[channel] = std::max(peaks[channel], std::abs(sample));
      square_sums[channel] += static_cast<double>(sample) * sample;
    }
  }
}

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
    const bool enqueue = attached_ && !recording_paused_ && !recording_stopped_;
    if (format_ && (*format_ != format || clock_ != clock)) {
      error = CaptureError{CaptureErrorCode::Discontinuity, "The microphone format or clock changed."};
    } else if (format != RequestedFormat(format)) {
      error = CaptureError{CaptureErrorCode::Unavailable, "The microphone cannot deliver the requested PCM format."};
    } else if (last_pts_ && pts.Compare(*last_pts_) != 1) {
      error = CaptureError{CaptureErrorCode::Discontinuity, "The microphone timestamp did not advance."};
    } else if (enqueue && (queue_.size() == max_pcm_blocks || pcm.size_bytes() > max_pcm_bytes - queued_bytes_)) {
      error = CaptureError{CaptureErrorCode::QueueOverflow, "The audio reader could not keep up with capture."};
    } else {
      if (input_level_) input_level_->Add(pcm, channels);
      if (enqueue) {
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
  input_level_.reset();
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

bool CaptureRun::SetInputLevelMonitoring(bool enabled) {
  std::lock_guard lock(mutex_);
  if (ended_ || input_level_.has_value() == enabled) return false;
  if (enabled) {
    input_level_.emplace();
  } else {
    input_level_.reset();
  }
  return true;
}

std::optional<AudioInputLevel> CaptureRun::TakeInputLevel() {
  InputLevelWindow window;
  AudioInputLevel level;
  {
    std::lock_guard lock(mutex_);
    if (!format_ || !input_level_ || input_level_->frames == 0) return {};
    window = std::exchange(*input_level_, {});
    level.channel_count = format_->ChannelCount();
  }
  const auto frames = static_cast<double>(window.frames);
  for (std::uint32_t channel = 0; channel < level.channel_count; ++channel) {
    level.channels[channel] = {
        .peak = static_cast<float>(window.peaks[channel] / pcm_full_scale),
        .rms = static_cast<float>(std::sqrt(window.square_sums[channel] / frames) / pcm_full_scale),
    };
  }
  return level;
}

CaptureResult<AudioReader> CaptureRun::OpenReader(std::function<void()> recording_released) {
  std::lock_guard lock(mutex_);
  if (ended_)
    return CaptureResult<AudioReader>::Failure({CaptureErrorCode::NotReady, "This capture run has ended."});
  if (attached_)
    return CaptureResult<AudioReader>::Failure({CaptureErrorCode::ReaderInUse, "This run already has an audio reader."});
  const auto next_reader_id = reader_id_ + 1;
  auto reader = AudioReader(shared_from_this(), next_reader_id);
  attached_ = true;
  recording_released_ = std::move(recording_released);
  reader_id_ = next_reader_id;
  return CaptureResult<AudioReader>::Success(std::move(reader));
}

void CaptureRun::ReleaseReader(std::uint64_t reader_id) {
  std::shared_ptr<CallbackCompletion<ReadResult>> completion;
  std::function<void()> recording_released;
  {
    std::lock_guard lock(mutex_);
    if (!attached_ || reader_id_ != reader_id) return;
    queue_.clear();
    queued_bytes_ = 0;
    attached_ = false;
    recording_paused_ = false;
    recording_stopped_ = false;
    recording_released = std::exchange(recording_released_, {});
    completion = std::exchange(waiting_read_, {});
  }
  changed_.notify_all();
  if (completion) {
    completion->Complete(ReadResult::Failure({CaptureErrorCode::Interrupted, "The audio reader was released."}));
  }
  if (recording_released) recording_released();
}

void CaptureRun::SetRecordingPaused(bool paused) {
  std::lock_guard lock(mutex_);
  if (!ended_ && !recording_stopped_ && recording_released_) recording_paused_ = paused;
}

bool CaptureRun::RecordingPaused() const {
  std::lock_guard lock(mutex_);
  return !ended_ && recording_paused_;
}

void CaptureRun::StopRecording() {
  {
    std::lock_guard lock(mutex_);
    if (ended_ || recording_stopped_ || !recording_released_) return;
    recording_stopped_ = true;
    recording_paused_ = false;
  }
  changed_.notify_all();
  CompleteRead();
}

ReadResult CaptureRun::Read(std::uint64_t reader_id, std::stop_token cancellation) {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, cancellation, [this, reader_id] {
    return ReadReadyLocked() || !attached_ || reader_id_ != reader_id;
  });
  if (cancellation.stop_requested())
    return ReadResult::Failure({CaptureErrorCode::Interrupted, "Audio reading was canceled."});
  if (!attached_ || reader_id_ != reader_id)
    return ReadResult::Failure({CaptureErrorCode::Interrupted, "The audio reader was released."});
  return TakeReadLocked();
}

bool CaptureRun::ReadReadyLocked() const { return ended_ || recording_stopped_ || !queue_.empty(); }

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
  if (!waiting_read_ || !ReadReadyLocked()) return;
  auto result = TakeReadLocked();
  auto completion = std::exchange(waiting_read_, {});
  lock.unlock();
  completion->Complete(std::move(result));
}

Task<ReadResult> CaptureRun::ReadAsync(std::uint64_t reader_id) {
  return AwaitCallback<ReadResult>(
      [run = shared_from_this(), reader_id](CallbackCompletion<ReadResult> completion)
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
