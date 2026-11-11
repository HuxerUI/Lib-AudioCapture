// Use the NDK declarations for types only. AAudio entry points are loaded dynamically below.
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
#include <aaudio/AAudio.h>
#include <dlfcn.h>

#include <array>
#include <thread>

#include "capture_run.h"

namespace huxerui::audio::detail {
namespace {

// Resolve AAudio at runtime so apps can still load the library on Android API 23–25.
#define HUX_AAUDIO_SYMBOLS(X) \
  X(AAudio_createStreamBuilder) \
  X(AAudioStreamBuilder_delete) \
  X(AAudioStreamBuilder_setDirection) \
  X(AAudioStreamBuilder_setFormat) \
  X(AAudioStreamBuilder_setSampleRate) \
  X(AAudioStreamBuilder_setChannelCount) \
  X(AAudioStreamBuilder_setBufferCapacityInFrames) \
  X(AAudioStreamBuilder_openStream) \
  X(AAudioStream_close) \
  X(AAudioStream_requestStart) \
  X(AAudioStream_requestStop) \
  X(AAudioStream_getSampleRate) \
  X(AAudioStream_getFormat) \
  X(AAudioStream_getChannelCount) \
  X(AAudioStream_getFramesRead) \
  X(AAudioStream_getTimestamp) \
  X(AAudioStream_getXRunCount) \
  X(AAudioStream_read) \
  X(AAudio_convertResultToText)

struct AAudioApi {
  void* library = dlopen("libaaudio.so", RTLD_LOCAL | RTLD_NOW);
#define HUX_DECLARE_AAUDIO(name) \
  decltype(&::name) name = library ? reinterpret_cast<decltype(&::name)>(dlsym(library, #name)) : nullptr;
  HUX_AAUDIO_SYMBOLS(HUX_DECLARE_AAUDIO)
#undef HUX_DECLARE_AAUDIO

  ~AAudioApi() {
    if (library) dlclose(library);
  }

  bool Available() const {
#define HUX_CHECK_AAUDIO(name) && name
    return library HUX_AAUDIO_SYMBOLS(HUX_CHECK_AAUDIO);
#undef HUX_CHECK_AAUDIO
  }

  CaptureError Error(const char* action, aaudio_result_t result) const {
    const auto code = result == AAUDIO_ERROR_DISCONNECTED ? CaptureErrorCode::Interrupted
                                                          : CaptureErrorCode::CaptureFailed;
    return {code, std::string(action) + ": " + AAudio_convertResultToText(result)};
  }
};

#undef HUX_AAUDIO_SYMBOLS

} // namespace

std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run) {
  using namespace std::chrono_literals;
  if (run->StopToken().stop_requested()) return {};
  static const AAudioApi api;
  if (!api.Available())
    return CaptureError{CaptureErrorCode::Unavailable, "Microphone capture requires Android API 26 or later."};

  AAudioStreamBuilder* raw_builder = nullptr;
  auto result = api.AAudio_createStreamBuilder(&raw_builder);
  if (result != AAUDIO_OK) return api.Error("Cannot create the microphone stream builder", result);
  std::unique_ptr<AAudioStreamBuilder, decltype(api.AAudioStreamBuilder_delete)>
      builder(raw_builder, api.AAudioStreamBuilder_delete);
  api.AAudioStreamBuilder_setDirection(builder.get(), AAUDIO_DIRECTION_INPUT);
  api.AAudioStreamBuilder_setFormat(builder.get(), AAUDIO_FORMAT_PCM_I16);
  api.AAudioStreamBuilder_setSampleRate(builder.get(), mono_48k.sample_rate);
  api.AAudioStreamBuilder_setChannelCount(builder.get(), 1);
  api.AAudioStreamBuilder_setBufferCapacityInFrames(builder.get(), 4800);

  AAudioStream* raw_stream = nullptr;
  result = api.AAudioStreamBuilder_openStream(builder.get(), &raw_stream);
  if (result != AAUDIO_OK) return api.Error("Cannot open the microphone", result);
  builder.reset();
  const auto close = [](AAudioStream* stream) {
    api.AAudioStream_requestStop(stream);
    api.AAudioStream_close(stream);
  };
  std::unique_ptr<AAudioStream, decltype(close)> stream(raw_stream, close);
  if (api.AAudioStream_getSampleRate(stream.get()) != mono_48k.sample_rate ||
      api.AAudioStream_getChannelCount(stream.get()) != 1 ||
      api.AAudioStream_getFormat(stream.get()) != AAUDIO_FORMAT_PCM_I16)
    return CaptureError{CaptureErrorCode::Unavailable, "The microphone cannot supply mono 48 kHz S16 PCM."};

  if (run->StopToken().stop_requested()) return {};
  result = api.AAudioStream_requestStart(stream.get());
  if (result != AAUDIO_OK) return api.Error("Cannot start the microphone", result);

  std::array<std::int16_t, 480> pcm;
  std::optional<std::int64_t> previous_end;
  std::int32_t previous_xruns = 0;
  auto last_pcm = std::chrono::steady_clock::now();
  const auto stop = run->StopToken();
  while (!stop.stop_requested()) {
    if (std::chrono::steady_clock::now() - last_pcm > 3s)
      return CaptureError{CaptureErrorCode::CaptureFailed, "The microphone produced no timestamped PCM."};

    // A nonblocking read keeps stop responsive on devices that ignore a positive timeout.
    const auto count = api.AAudioStream_read(stream.get(), pcm.data(), pcm.size(), 0);
    if (count == 0 || (!previous_end && count == AAUDIO_ERROR_INVALID_STATE)) {
      std::this_thread::sleep_for(5ms);
      continue;
    }
    if (count < 0) return api.Error("Cannot read microphone PCM", count);

    const auto end = api.AAudioStream_getFramesRead(stream.get());
    const auto xruns = api.AAudioStream_getXRunCount(stream.get());
    if (end < count || count > static_cast<aaudio_result_t>(pcm.size()))
      return CaptureError{CaptureErrorCode::CaptureFailed, "Invalid AAudio sample position."};
    const auto first = end - count;
    if (previous_end && (first != *previous_end || xruns != previous_xruns))
      return CaptureError{CaptureErrorCode::Discontinuity, "AAudio lost microphone samples."};

    std::int64_t anchor_frame = 0, anchor_ns = 0;
    result = api.AAudioStream_getTimestamp(stream.get(), CLOCK_BOOTTIME, &anchor_frame, &anchor_ns);
    if (!previous_end && result == AAUDIO_ERROR_INVALID_STATE) continue;
    if (result != AAUDIO_OK) return api.Error("Cannot read the microphone clock", result);
    const auto pts = SampleTimestamp(first, anchor_frame, anchor_ns, mono_48k.sample_rate);
    if (!pts) return CaptureError{CaptureErrorCode::CaptureFailed, "Invalid AAudio clock correlation."};

    if (!run->Publish(mono_48k, CaptureClock::BootTime, *pts,
                      std::span(pcm).first(static_cast<std::size_t>(count)))) break;
    previous_end = end;
    previous_xruns = xruns;
    last_pcm = std::chrono::steady_clock::now();
  }
  return {};
}

} // namespace huxerui::audio::detail
