#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "capture_run.h"

namespace huxerui::audio::detail {
namespace {

using emscripten::val;

struct WebCapture {
  std::shared_ptr<CaptureRun> run;
  CaptureCompleted completed;
  val control = val::undefined();
  std::vector<std::int16_t> pcm;
  std::optional<media::AudioFormat> input_format;
  std::optional<double> next_pts;
};

std::map<unsigned, WebCapture> captures;
unsigned next_capture_id = 0;
std::map<unsigned, CallbackCompletion<InputDevicesResult>> input_queries;
unsigned next_input_query_id = 0;

CaptureError BrowserError(const std::string& kind, const std::string& message) {
  auto code = CaptureErrorCode::CaptureFailed;
  if (kind == "unavailable") code = CaptureErrorCode::Unavailable;
  else if (kind == "permission") code = CaptureErrorCode::PermissionDenied;
  else if (kind == "interrupted") code = CaptureErrorCode::Interrupted;
  return {code, message};
}

bool Publish(unsigned id, std::uint32_t rate, std::uint32_t channels, double pts, const val& samples) {
  auto found = captures.find(id);
  if (found == captures.end()) return false;
  auto& capture = found->second;
  if (capture.run->StopToken().stop_requested()) return false;

  try {
    constexpr std::size_t max_samples = 256 * 1024;
    const auto count = samples["length"].as<std::size_t>();
    if (!rate || (channels != 1 && channels != 2) || !count || count % channels || count > max_samples ||
        !std::isfinite(pts) || std::abs(pts) > 9'007'199'254'740'991.0 || std::trunc(pts) != pts) {
      capture.run->End(CaptureError{CaptureErrorCode::CaptureFailed, "Invalid browser microphone PCM or timestamp."});
      return false;
    }
    media::AudioFormat format{
        rate, media::AudioSampleFormat::S16, media::AudioPacking::Interleaved,
        channels == 1 ? media::AudioChannelLayout::Mono : media::AudioChannelLayout::Stereo};
    if (capture.input_format && *capture.input_format != format) {
      capture.run->End(CaptureError{CaptureErrorCode::Discontinuity, "The browser microphone format changed."});
      return false;
    }
    capture.input_format = format;
    format.channel_layout = capture.run->Options().channel_layout.value_or(format.channel_layout);
    const auto frames = count / channels;
    const auto output_channels = format.ChannelCount();
    if (!output_channels || frames > max_samples / output_channels) {
      capture.run->End(CaptureError{CaptureErrorCode::CaptureFailed, "Invalid converted browser microphone block."});
      return false;
    }
    constexpr std::uint32_t timescale = 1'000'000;
    const double duration = static_cast<double>(frames) * timescale / rate;
    // The browser may quantize capture times or report hardware clock jitter between adjacent blocks.
    if (capture.next_pts && std::abs(pts - *capture.next_pts) > std::max(5000.0, duration / 2)) {
      capture.run->End(CaptureError{CaptureErrorCode::Discontinuity, "The browser microphone sample timeline has a gap or overlap."});
      return false;
    }
    capture.next_pts = pts + duration;
    capture.pcm.resize(count);
    val(emscripten::typed_memory_view(count, capture.pcm.data())).call<void>("set", samples);
    if (output_channels < channels) {
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto sum = static_cast<std::int32_t>(capture.pcm[2 * frame]) + capture.pcm[2 * frame + 1];
        capture.pcm[frame] = static_cast<std::int16_t>(sum / 2);
      }
      capture.pcm.resize(frames);
    } else if (output_channels > channels) {
      capture.pcm.resize(frames * 2);
      // Expand backward to keep unread mono samples in place.
      for (std::size_t index = frames; index > 0; --index) {
        const auto frame = index - 1;
        const auto sample = capture.pcm[frame];
        capture.pcm[2 * frame] = sample;
        capture.pcm[2 * frame + 1] = sample;
      }
    }
    return capture.run->Publish(format, CaptureClock::MediaStreamTrackTime,
                                {static_cast<std::int64_t>(pts), timescale}, capture.pcm);
  } catch (const std::exception& error) {
    capture.run->End(CaptureError{CaptureErrorCode::CaptureFailed, error.what()});
    return false;
  }
}

void Finish(unsigned id, const std::string& kind, const std::string& message) {
  auto capture = captures.extract(id);
  if (capture.empty()) return;
  std::optional<CaptureError> error;
  if (!kind.empty()) error = BrowserError(kind, message);
  capture.mapped().completed(std::move(error));
}

void FinishInputQuery(unsigned id, const std::string& kind, const std::string& message, const val& inputs) {
  auto query = input_queries.extract(id);
  if (query.empty()) return;
  auto& completion = query.mapped();
  if (!kind.empty()) {
    completion.Complete(InputDevicesResult::Failure(BrowserError(kind, message)));
    return;
  }
  try {
    const auto count = inputs["length"].as<unsigned>();
    std::vector<InputDevice> devices;
    devices.reserve(count);
    for (unsigned index = 0; index < count; ++index) {
      const auto input = inputs[index];
      devices.push_back({input["id"].as<std::string>(), input["name"].as<std::string>()});
    }
    completion.Complete(InputDevicesResult::Success(std::move(devices)));
  } catch (const std::exception& error) {
    completion.Complete(InputDevicesResult::Failure({CaptureErrorCode::CaptureFailed, error.what()}));
  }
}

EMSCRIPTEN_BINDINGS(huxerui_audio_capture) {
  emscripten::function("huxeruiAudioCapturePublish", &Publish);
  emscripten::function("huxeruiAudioCaptureFinish", &Finish);
  emscripten::function("huxeruiAudioCaptureDevices", &FinishInputQuery);
}

} // namespace

std::function<void()> StartWebCapture(const std::shared_ptr<CaptureRun>& run, CaptureCompleted completed) {
  do { ++next_capture_id; } while (!next_capture_id || captures.contains(next_capture_id));
  const unsigned id = next_capture_id;
  std::function<void()> stop = [id] {
    auto capture = captures.extract(id);
    if (capture.empty()) return;
    auto& value = capture.mapped();
    value.control.call<void>("stop");
    value.completed(std::nullopt);
  };
  const auto capture = captures.emplace(id, WebCapture{run, std::move(completed)}).first;
  try {
    const auto& options = run->Options();
    const auto channels = options.channel_layout
        ? media::AudioFormat{.channel_layout = *options.channel_layout}.ChannelCount() : 0u;
    capture->second.control = val::global("HuxerAudioCapture").call<val>(
        "start", id, options.sample_rate.value_or(0), channels, options.device_id.value_or(std::string{}));
  } catch (...) {
    captures.erase(id);
    throw;
  }
  return stop;
}

Task<InputDevicesResult> EnumerateWebInputDevices() {
  return AwaitCallback<InputDevicesResult>([](CallbackCompletion<InputDevicesResult> completion) -> std::function<void()> {
    do { ++next_input_query_id; } while (!next_input_query_id || input_queries.contains(next_input_query_id));
    const auto id = next_input_query_id;
    input_queries.emplace(id, std::move(completion));
    try {
      auto control = val::global("HuxerAudioCapture").call<val>("enumerate", id);
      return [id, control = std::move(control)] {
        input_queries.erase(id);
        control.call<void>("stop");
      };
    } catch (...) {
      input_queries.erase(id);
      throw;
    }
  });
}

} // namespace huxerui::audio::detail
