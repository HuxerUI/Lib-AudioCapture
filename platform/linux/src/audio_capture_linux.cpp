#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string_view>

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

std::optional<media::MediaTimestamp> FirstSampleTime(const snd_pcm_status_t* status, snd_pcm_uframes_t available,
                                                     std::uint32_t sample_rate) {
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
  return SampleTimestamp(0, delay, anchor, sample_rate);
}

} // namespace

InputDevicesResult EnumeratePlatformInputDevices() {
  void** raw_hints = nullptr;
  const auto result = snd_device_name_hint(-1, "pcm", &raw_hints);
  if (result < 0) return InputDevicesResult::Failure(AlsaError("Cannot list ALSA input devices", result));
  std::unique_ptr<void*, decltype(&snd_device_name_free_hint)> hints(raw_hints, snd_device_name_free_hint);
  std::vector<InputDevice> devices;
  for (auto hint = hints.get(); hint && *hint; ++hint) {
    const std::unique_ptr<char, decltype(&std::free)> direction(snd_device_name_get_hint(*hint, "IOID"), std::free);
    if (direction && std::string_view(direction.get()) != "Input") continue;
    const std::unique_ptr<char, decltype(&std::free)> name(snd_device_name_get_hint(*hint, "NAME"), std::free);
    if (!name || !*name) continue;
    const std::string_view id(name.get());
    if (id == "default" || id == "null" || std::ranges::find(devices, id, &InputDevice::id) != devices.end()) continue;
    const std::unique_ptr<char, decltype(&std::free)> description(snd_device_name_get_hint(*hint, "DESC"), std::free);
    const std::string_view label = description ? description.get() : "";
    const auto first_line = label.substr(0, label.find('\n'));
    devices.push_back({std::string(id), std::string(first_line.empty() ? id : first_line)});
  }
  return InputDevicesResult::Success(std::move(devices));
}

std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run) {
  using namespace std::chrono_literals;
  const auto stop = run->StopToken();
  if (stop.stop_requested()) return {};

  const auto& id = run->Options().device_id;
  if (id) {
    const auto inputs = EnumeratePlatformInputDevices();
    if (!inputs.Succeeded()) return inputs.Error();
    const auto& devices = inputs.Value();
    if (std::ranges::find(devices, *id, &InputDevice::id) == devices.end())
      return CaptureError{CaptureErrorCode::Unavailable, "The selected ALSA input device is unavailable."};
  }
  if (stop.stop_requested()) return {};
  snd_pcm_t* device = nullptr;
  auto result = snd_pcm_open(&device, id ? id->c_str() : "default", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
  if (result < 0) {
    auto error = AlsaError("Cannot open the microphone", result);
    if (id && (result == -EBUSY || result == -ENXIO)) error.code = CaptureErrorCode::Unavailable;
    return error;
  }
  std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> stream(device, snd_pcm_close);

  const auto format = run->RequestedFormat(mono_48k);
  const auto channels = format.ChannelCount();
  result = snd_pcm_set_params(device, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                              channels, format.sample_rate, 1, 100'000);
  if (result < 0) {
    auto error = AlsaError("Cannot configure the requested S16 PCM format", result);
    if (result == -EINVAL) error.code = CaptureErrorCode::Unavailable;
    return error;
  }
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
  constexpr snd_pcm_uframes_t frames_per_read = 480;
  std::array<std::int16_t, frames_per_read * 2> samples;
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
    const auto pts = FirstSampleTime(status, available, format.sample_rate);
    if (!pts) return CaptureError{CaptureErrorCode::CaptureFailed, "Invalid ALSA clock correlation."};

    const auto count = snd_pcm_readi(device, samples.data(), std::min(available, frames_per_read));
    if (!count || count == -EAGAIN || count == -EINTR) continue;
    if (count < 0) return AlsaError("Cannot read microphone samples", static_cast<int>(count));
    if (!run->Publish(format, CaptureClock::Monotonic, *pts,
                      std::span(samples).first(static_cast<std::size_t>(count) * channels))) break;
    last_pcm = std::chrono::steady_clock::now();
  }
  return {};
}

} // namespace huxerui::audio::detail
