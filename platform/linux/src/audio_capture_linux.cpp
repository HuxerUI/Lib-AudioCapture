#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>

#include "capture_run.h"

namespace huxerui::audio::detail {
namespace {

constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;

CaptureError AlsaError(const char* action, int error) {
  auto code = CaptureErrorCode::CaptureFailed;
  if (error == -EACCES || error == -EPERM) {
    code = CaptureErrorCode::PermissionDenied;
  } else if (error == -ENOENT || error == -ENODEV || error == -ENOTSUP) {
    code = CaptureErrorCode::Unavailable;
  } else if (error == -EPIPE) {
    code = CaptureErrorCode::Discontinuity;
  } else if (error == -ESTRPIPE) {
    code = CaptureErrorCode::Interrupted;
  }
  return {code, std::string(action) + ": " + snd_strerror(error)};
}

std::optional<media::MediaTimestamp> FirstSampleTime(const snd_pcm_status_t* status, snd_pcm_uframes_t available) {
  snd_htimestamp_t time{};
  snd_pcm_status_get_htstamp(status, &time);
  if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= nanoseconds_per_second ||
      time.tv_sec > (std::numeric_limits<std::int64_t>::max() - time.tv_nsec) / nanoseconds_per_second ||
      available > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return {};
  }
  // The first unread frame precedes the clock snapshot by the queued frames and any input latency ALSA reports.
  const auto delay = std::max<std::int64_t>(snd_pcm_status_get_delay(status), available);
  const auto anchor = static_cast<std::int64_t>(time.tv_sec) * nanoseconds_per_second + time.tv_nsec;
  return SampleTimestamp(0, delay, anchor, mono_48k.sample_rate);
}

} // namespace

std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run) {
  using namespace std::chrono_literals;
  const auto stop = run->StopToken();
  if (stop.stop_requested()) return {};

  snd_pcm_t* device = nullptr;
  auto result = snd_pcm_open(&device, "default", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
  if (result < 0) return AlsaError("Cannot open the default microphone", result);
  std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> stream(device, snd_pcm_close);

  result = snd_pcm_set_params(device, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                              1, mono_48k.sample_rate, 1, 100'000);
  if (result < 0) return AlsaError("Cannot configure mono 48 kHz microphone capture", result);
  snd_pcm_uframes_t buffer_frames = 0, period_frames = 0;
  result = snd_pcm_get_params(device, &buffer_frames, &period_frames);
  if (result < 0) return AlsaError("Cannot query the microphone buffer", result);

  snd_pcm_sw_params_t* params = nullptr;
  snd_pcm_sw_params_alloca(&params);
  result = snd_pcm_sw_params_current(device, params);
  if (result >= 0) result = snd_pcm_sw_params_set_tstamp_mode(device, params, SND_PCM_TSTAMP_ENABLE);
  if (result >= 0) result = snd_pcm_sw_params_set_tstamp_type(device, params, SND_PCM_TSTAMP_TYPE_MONOTONIC);
  if (result >= 0) result = snd_pcm_sw_params(device, params);
  if (result < 0) return AlsaError("Cannot enable monotonic microphone timestamps", result);

  if (stop.stop_requested()) return {};
  result = snd_pcm_start(device);
  if (result < 0) return AlsaError("Cannot start the microphone", result);

  snd_pcm_status_t* status = nullptr;
  snd_pcm_status_alloca(&status);
  std::array<std::int16_t, 480> samples;
  auto last_pcm = std::chrono::steady_clock::now();
  while (!stop.stop_requested()) {
    if (std::chrono::steady_clock::now() - last_pcm > 3s)
      return CaptureError{CaptureErrorCode::CaptureFailed, "The microphone produced no timestamped PCM."};

    result = snd_pcm_wait(device, 50);
    if (!result || result == -EINTR || result == -EAGAIN) continue;
    if (result < 0) return AlsaError("Cannot wait for microphone samples", result);

    const auto ready = snd_pcm_avail_update(device);
    if (!ready || ready == -EAGAIN || ready == -EINTR) continue;
    if (ready < 0) return AlsaError("Cannot query microphone samples", static_cast<int>(ready));

    result = snd_pcm_status(device, status);
    if (result == -EAGAIN || result == -EINTR) continue;
    if (result < 0) return AlsaError("Cannot read the microphone clock", result);
    const auto state = snd_pcm_status_get_state(status);
    if (state == SND_PCM_STATE_SUSPENDED || state == SND_PCM_STATE_DISCONNECTED)
      return CaptureError{CaptureErrorCode::Interrupted, "The microphone was suspended or disconnected."};
    const auto available = snd_pcm_status_get_avail(status);
    if (state == SND_PCM_STATE_XRUN || available > buffer_frames)
      return CaptureError{CaptureErrorCode::Discontinuity, "ALSA lost microphone samples."};
    if (!available) continue;
    const auto pts = FirstSampleTime(status, available);
    if (!pts) return CaptureError{CaptureErrorCode::CaptureFailed, "Invalid ALSA clock correlation."};

    const auto count = snd_pcm_readi(device, samples.data(),
                                     std::min(available, static_cast<snd_pcm_uframes_t>(samples.size())));
    if (!count || count == -EAGAIN || count == -EINTR) continue;
    if (count < 0) return AlsaError("Cannot read microphone samples", static_cast<int>(count));
    if (!run->Publish(mono_48k, CaptureClock::Monotonic, *pts,
                      std::span(samples).first(static_cast<std::size_t>(count)))) break;
    last_pcm = std::chrono::steady_clock::now();
  }
  return {};
}

} // namespace huxerui::audio::detail
