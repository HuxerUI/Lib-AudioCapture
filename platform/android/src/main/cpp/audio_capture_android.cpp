// Use the NDK declarations for types only. AAudio entry points are loaded dynamically below.
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
#include <aaudio/AAudio.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <thread>

#include <huxerui/android/platform_registry.h>

#include "capture_run.h"

namespace huxerui::audio::detail {
namespace {

// Resolve AAudio at runtime so apps can still load the library on Android API 23–25.
#define HUX_AAUDIO_SYMBOLS(X) \
  X(AAudio_createStreamBuilder) \
  X(AAudioStreamBuilder_delete) \
  X(AAudioStreamBuilder_setDirection) \
  X(AAudioStreamBuilder_setDeviceId) \
  X(AAudioStreamBuilder_setFormat) \
  X(AAudioStreamBuilder_setSampleRate) \
  X(AAudioStreamBuilder_setChannelCount) \
  X(AAudioStreamBuilder_setBufferCapacityInFrames) \
  X(AAudioStreamBuilder_openStream) \
  X(AAudioStream_close) \
  X(AAudioStream_requestStart) \
  X(AAudioStream_requestStop) \
  X(AAudioStream_getDeviceId) \
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
    auto code = CaptureErrorCode::CaptureFailed;
    if (result == AAUDIO_ERROR_DISCONNECTED) code = CaptureErrorCode::Interrupted;
    else if (result == AAUDIO_ERROR_INVALID_RATE || result == AAUDIO_ERROR_UNAVAILABLE) code = CaptureErrorCode::Unavailable;
    return {code, std::string(action) + ": " + AAudio_convertResultToText(result)};
  }
};

#undef HUX_AAUDIO_SYMBOLS

const AAudioApi& AAudio() {
  static const AAudioApi api;
  return api;
}

class JniThread {
public:
  explicit JniThread(JavaVM* vm) : vm_(vm) {
    void* current = nullptr;
    const auto status = vm_->GetEnv(&current, JNI_VERSION_1_6);
    if (status == JNI_OK) environment = static_cast<JNIEnv*>(current);
    else if (status == JNI_EDETACHED) attached_ = vm_->AttachCurrentThread(&environment, nullptr) == JNI_OK;
  }

  ~JniThread() {
    if (attached_) vm_->DetachCurrentThread();
  }

  JNIEnv* environment = nullptr;

private:
  JavaVM* vm_;
  bool attached_ = false;
};

void CheckJni(JNIEnv& environment) {
  if (environment.ExceptionCheck()) {
    environment.ExceptionClear();
    throw std::runtime_error("Android could not query microphone devices.");
  }
}

template<class T>
T RequireJni(JNIEnv& environment, T value) {
  CheckJni(environment);
  if (!value) throw std::runtime_error("Android returned a missing microphone service or device.");
  return value;
}

class AndroidInputEnumerator {
public:
  explicit AndroidInputEnumerator(android::PlatformEnv env) {
    if (!env.jni || !env.context || env.jni->GetJavaVM(&vm_) != JNI_OK)
      throw std::runtime_error("Cannot access the Android application context for microphone discovery.");
    context_ = RequireJni(*env.jni, env.jni->NewGlobalRef(env.context));
  }

  AndroidInputEnumerator(const AndroidInputEnumerator&) = delete;
  AndroidInputEnumerator& operator=(const AndroidInputEnumerator&) = delete;

  ~AndroidInputEnumerator() {
    const JniThread thread(vm_);
    if (thread.environment) thread.environment->DeleteGlobalRef(context_);
  }

  InputDevicesResult Query() const {
    if (!AAudio().Available()) {
      return InputDevicesResult::Failure({CaptureErrorCode::Unavailable,
          "Microphone capture requires Android API 26 or later."});
    }
    const JniThread thread(vm_);
    if (!thread.environment)
      return InputDevicesResult::Failure({CaptureErrorCode::CaptureFailed, "Cannot attach the microphone discovery worker."});
    auto& env = *thread.environment;
    using android::LocalRef;
    const LocalRef context_class(&env, RequireJni(env, env.GetObjectClass(context_)));
    const auto service_method = RequireJni(env, env.GetMethodID(
        context_class.Get(), "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"));
    const LocalRef service_name(&env, RequireJni(env, env.NewStringUTF("audio")));
    const LocalRef manager(&env, RequireJni(env, env.CallObjectMethod(context_, service_method, service_name.Get())));
    const LocalRef manager_class(&env, RequireJni(env, env.GetObjectClass(manager.Get())));
    const auto devices_method = RequireJni(env, env.GetMethodID(
        manager_class.Get(), "getDevices", "(I)[Landroid/media/AudioDeviceInfo;"));
    constexpr jint get_devices_inputs = 1; // AudioManager.GET_DEVICES_INPUTS
    const LocalRef devices(&env, static_cast<jobjectArray>(
        RequireJni(env, env.CallObjectMethod(manager.Get(), devices_method, get_devices_inputs))));

    const LocalRef device_class(&env, RequireJni(env, env.FindClass("android/media/AudioDeviceInfo")));
    const auto id_method = RequireJni(env, env.GetMethodID(device_class.Get(), "getId", "()I"));
    const auto name_method = RequireJni(env, env.GetMethodID(
        device_class.Get(), "getProductName", "()Ljava/lang/CharSequence;"));
    const LocalRef sequence_class(&env, RequireJni(env, env.FindClass("java/lang/CharSequence")));
    const auto string_method = RequireJni(env, env.GetMethodID(sequence_class.Get(), "toString", "()Ljava/lang/String;"));
    const auto count = env.GetArrayLength(devices.Get());
    std::vector<InputDevice> inputs;
    inputs.reserve(count);
    for (jsize index = 0; index < count; ++index) {
      const LocalRef device(&env, RequireJni(env, env.GetObjectArrayElement(devices.Get(), index)));
      const auto id = env.CallIntMethod(device.Get(), id_method);
      CheckJni(env);
      if (id <= AAUDIO_UNSPECIFIED) continue;
      const LocalRef name(&env, env.CallObjectMethod(device.Get(), name_method));
      CheckJni(env);
      std::string label;
      if (name) {
        const LocalRef text(&env, static_cast<jstring>(env.CallObjectMethod(name.Get(), string_method)));
        CheckJni(env);
        if (text) label = android::JavaStringToUtf8(&env, text.Get());
      }
      if (label.empty()) label = "Microphone " + std::to_string(id);
      inputs.push_back({std::to_string(id), std::move(label)});
    }
    return InputDevicesResult::Success(std::move(inputs));
  }

private:
  JavaVM* vm_ = nullptr;
  jobject context_ = nullptr;
};

} // namespace

std::function<Task<InputDevicesResult>()> MakeAndroidInputEnumerator(Runtime& runtime) {
  auto enumerator = std::make_shared<AndroidInputEnumerator>(android::GetPlatformEnv(runtime));
  return [enumerator = std::move(enumerator)] { return RunWorker([enumerator] { return enumerator->Query(); }); };
}

std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run) {
  using namespace std::chrono_literals;
  if (run->StopToken().stop_requested()) return {};
  const auto& api = AAudio();
  if (!api.Available())
    return CaptureError{CaptureErrorCode::Unavailable, "Microphone capture requires Android API 26 or later."};

  const auto& id = run->Options().device_id;
  std::int32_t device_id = AAUDIO_UNSPECIFIED;
  if (id) {
    const auto end = id->data() + id->size();
    const auto parsed = std::from_chars(id->data(), end, device_id);
    if (parsed.ec != std::errc{} || parsed.ptr != end || device_id <= AAUDIO_UNSPECIFIED)
      return CaptureError{CaptureErrorCode::Unavailable, "The requested microphone ID is invalid."};
  }
  const auto format = run->RequestedFormat(mono_48k);
  const auto rate = static_cast<std::int32_t>(format.sample_rate);
  const auto channels = static_cast<std::int32_t>(format.ChannelCount());
  AAudioStreamBuilder* raw_builder = nullptr;
  auto result = api.AAudio_createStreamBuilder(&raw_builder);
  if (result != AAUDIO_OK) return api.Error("Cannot create the microphone stream builder", result);
  std::unique_ptr<AAudioStreamBuilder, decltype(api.AAudioStreamBuilder_delete)>
      builder(raw_builder, api.AAudioStreamBuilder_delete);
  api.AAudioStreamBuilder_setDirection(builder.get(), AAUDIO_DIRECTION_INPUT);
  if (id) api.AAudioStreamBuilder_setDeviceId(builder.get(), device_id);
  api.AAudioStreamBuilder_setFormat(builder.get(), AAUDIO_FORMAT_PCM_I16);
  api.AAudioStreamBuilder_setSampleRate(builder.get(), rate);
  api.AAudioStreamBuilder_setChannelCount(builder.get(), channels);
  api.AAudioStreamBuilder_setBufferCapacityInFrames(builder.get(), std::max(1, rate / 10));

  AAudioStream* raw_stream = nullptr;
  result = api.AAudioStreamBuilder_openStream(builder.get(), &raw_stream);
  if (result != AAUDIO_OK) {
    auto error = api.Error("Cannot open the microphone", result);
    if (id && (result == AAUDIO_ERROR_DISCONNECTED || result == AAUDIO_ERROR_ILLEGAL_ARGUMENT))
      error.code = CaptureErrorCode::Unavailable;
    return error;
  }
  builder.reset();
  const auto close = [&api](AAudioStream* stream) {
    api.AAudioStream_requestStop(stream);
    api.AAudioStream_close(stream);
  };
  std::unique_ptr<AAudioStream, decltype(close)> stream(raw_stream, close);
  const auto opened_device = id ? api.AAudioStream_getDeviceId(stream.get()) : AAUDIO_UNSPECIFIED;
  if (id && opened_device != AAUDIO_UNSPECIFIED && opened_device != device_id)
    return CaptureError{CaptureErrorCode::Unavailable, "Android could not route capture to the selected microphone."};
  if (api.AAudioStream_getSampleRate(stream.get()) != rate ||
      api.AAudioStream_getChannelCount(stream.get()) != channels ||
      api.AAudioStream_getFormat(stream.get()) != AAUDIO_FORMAT_PCM_I16)
    return CaptureError{CaptureErrorCode::Unavailable, "The microphone cannot supply the requested S16 PCM format."};

  if (run->StopToken().stop_requested()) return {};
  result = api.AAudioStream_requestStart(stream.get());
  if (result != AAUDIO_OK) return api.Error("Cannot start the microphone", result);

  constexpr std::int32_t frames_per_read = 480;
  std::array<std::int16_t, frames_per_read * 2> pcm;
  std::optional<std::int64_t> previous_end;
  std::int32_t previous_xruns = 0;
  auto last_pcm = std::chrono::steady_clock::now();
  const auto stop = run->StopToken();
  while (!stop.stop_requested()) {
    if (std::chrono::steady_clock::now() - last_pcm > 3s) {
      if (id && api.AAudioStream_getDeviceId(stream.get()) != device_id)
        return CaptureError{CaptureErrorCode::Unavailable, "Android could not route capture to the selected microphone."};
      return CaptureError{CaptureErrorCode::CaptureFailed, "The microphone produced no timestamped PCM."};
    }

    // A nonblocking read keeps stop responsive on devices that ignore a positive timeout.
    const auto count = api.AAudioStream_read(stream.get(), pcm.data(), frames_per_read, 0);
    if (count == 0 || (!previous_end && count == AAUDIO_ERROR_INVALID_STATE)) {
      std::this_thread::sleep_for(5ms);
      continue;
    }
    if (count < 0) return api.Error("Cannot read microphone PCM", count);
    if (id) {
      const auto routed_device = api.AAudioStream_getDeviceId(stream.get());
      if (routed_device == AAUDIO_UNSPECIFIED && !previous_end) continue;
      if (routed_device != device_id)
        return CaptureError{previous_end ? CaptureErrorCode::Interrupted : CaptureErrorCode::Unavailable,
                            "Android changed the selected microphone route."};
    }

    const auto end = api.AAudioStream_getFramesRead(stream.get());
    const auto xruns = api.AAudioStream_getXRunCount(stream.get());
    if (end < count || count > frames_per_read)
      return CaptureError{CaptureErrorCode::CaptureFailed, "Invalid AAudio sample position."};
    const auto first = end - count;
    if (previous_end && (first != *previous_end || xruns != previous_xruns))
      return CaptureError{CaptureErrorCode::Discontinuity, "AAudio lost microphone samples."};

    std::int64_t anchor_frame = 0, anchor_ns = 0;
    result = api.AAudioStream_getTimestamp(stream.get(), CLOCK_BOOTTIME, &anchor_frame, &anchor_ns);
    if (!previous_end && result == AAUDIO_ERROR_INVALID_STATE) continue;
    if (result != AAUDIO_OK) return api.Error("Cannot read the microphone clock", result);
    const auto pts = SampleTimestamp(first, anchor_frame, anchor_ns, format.sample_rate);
    if (!pts) return CaptureError{CaptureErrorCode::CaptureFailed, "Invalid AAudio clock correlation."};

    if (!run->Publish(format, CaptureClock::BootTime, *pts,
                      std::span(pcm).first(static_cast<std::size_t>(count) * channels))) break;
    previous_end = end;
    previous_xruns = xruns;
    last_pcm = std::chrono::steady_clock::now();
  }
  return {};
}

} // namespace huxerui::audio::detail
