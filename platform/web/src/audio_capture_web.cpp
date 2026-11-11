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
  std::optional<double> next_pts;
};

std::map<unsigned, WebCapture> captures;
unsigned next_capture_id = 0;

bool Publish(unsigned id, std::uint32_t rate, std::uint32_t channels, double pts, const val& samples) {
  auto found = captures.find(id);
  if (found == captures.end()) return false;
  auto& capture = found->second;
  if (capture.run->StopToken().stop_requested()) return false;

  try {
    const auto count = samples["length"].as<std::size_t>();
    if (!rate || (channels != 1 && channels != 2) || !count || count % channels || count > 256 * 1024 ||
        !std::isfinite(pts) || std::abs(pts) > 9'007'199'254'740'991.0 || std::trunc(pts) != pts) {
      capture.run->End(CaptureError{CaptureErrorCode::CaptureFailed, "Invalid browser microphone PCM or timestamp."});
      return false;
    }
    constexpr std::uint32_t timescale = 1'000'000;
    const double duration = static_cast<double>(count / channels) * timescale / rate;
    // The browser may quantize capture times or report hardware clock jitter between adjacent blocks.
    if (capture.next_pts && std::abs(pts - *capture.next_pts) > std::max(5000.0, duration / 2)) {
      capture.run->End(CaptureError{CaptureErrorCode::Discontinuity, "The browser microphone sample timeline has a gap or overlap."});
      return false;
    }
    capture.next_pts = pts + duration;
    capture.pcm.resize(count);
    val(emscripten::typed_memory_view(count, capture.pcm.data())).call<void>("set", samples);
    return capture.run->Publish(
        {rate, media::AudioSampleFormat::S16, media::AudioPacking::Interleaved,
         channels == 1 ? media::AudioChannelLayout::Mono : media::AudioChannelLayout::Stereo},
        CaptureClock::MediaStreamTrackTime, {static_cast<std::int64_t>(pts), timescale}, capture.pcm);
  } catch (const std::exception& error) {
    capture.run->End(CaptureError{CaptureErrorCode::CaptureFailed, error.what()});
    return false;
  }
}

void Finish(unsigned id, const std::string& kind, const std::string& message) {
  auto capture = captures.extract(id);
  if (capture.empty()) return;
  std::optional<CaptureError> error;
  if (!kind.empty()) {
    auto code = CaptureErrorCode::CaptureFailed;
    if (kind == "unavailable") code = CaptureErrorCode::Unavailable;
    else if (kind == "permission") code = CaptureErrorCode::PermissionDenied;
    else if (kind == "interrupted") code = CaptureErrorCode::Interrupted;
    error = CaptureError{code, message};
  }
  capture.mapped().completed(std::move(error));
}

EMSCRIPTEN_BINDINGS(huxerui_audio_capture) {
  emscripten::function("huxeruiAudioCapturePublish", &Publish);
  emscripten::function("huxeruiAudioCaptureFinish", &Finish);
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
    capture->second.control = val::global("HuxerAudioCapture").call<val>("start", id);
  } catch (...) {
    captures.erase(id);
    throw;
  }
  return stop;
}

} // namespace huxerui::audio::detail
