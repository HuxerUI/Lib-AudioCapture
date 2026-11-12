#include "capture_run.h"

#include <chrono>
#include <limits>
#include <thread>
#include <utility>

#include <huxerui/lifecycle.h>
#include <huxerui/state.h>
#include <huxerui/system.h>

namespace huxerui::audio::detail {
namespace {

bool CaptureSettingsChanged(const CaptureOptions& previous, const CaptureOptions& next) {
  return previous.active != next.active || previous.sample_rate != next.sample_rate ||
         previous.channel_layout != next.channel_layout || previous.device_id != next.device_id ||
         previous.pcm_processor != next.pcm_processor;
}

CaptureError PermissionError(PermissionStatus permission) {
  if (permission == PermissionStatus::NotDetermined)
    return {CaptureErrorCode::PermissionRequired, "Microphone permission has not been granted."};
  if (permission == PermissionStatus::Unavailable)
    return {CaptureErrorCode::Unavailable, "Microphone access is unavailable."};
  return {CaptureErrorCode::PermissionDenied, "Microphone access is denied."};
}

#if defined(HUXERUI_AUDIO_CAPTURE_AVAILABLE) && !defined(__EMSCRIPTEN__)
std::function<void()> StartCaptureWorker(const std::shared_ptr<CaptureRun>& run, CaptureCompleted completed) {
  std::thread([run, completed = std::move(completed)] {
    std::optional<CaptureError> error;
    try {
      error = RunPlatformCapture(run);
    } catch (const std::exception& exception) {
      error = CaptureError{CaptureErrorCode::CaptureFailed, exception.what()};
    }
    completed(std::move(error));
  }).detach();
  return {};
}
#endif

} // namespace

struct Session : std::enable_shared_from_this<Session> {
  State<CaptureStatus> status;
  State<std::optional<AudioInputLevel>> input_level;
  TaskScope tasks;
  ApplicationHandle application;
  CaptureBackend backend;
  std::shared_ptr<CaptureRun> run;
  std::function<void()> stop_capture;
  TaskHandle permission;
  TaskHandle input_level_updates;
  std::optional<CaptureError> failure;
  std::uint64_t generation = 0;
  CaptureOptions options;
  bool background = false;
  bool producer = false;
  bool closed = false;

  Session(State<CaptureStatus> status, State<std::optional<AudioInputLevel>> input_level,
          TaskScope tasks, ApplicationHandle app)
      : status(status), input_level(input_level), tasks(tasks), application(app) {}

  void Reconcile() {
    if (closed || status->state == SessionState::Stopping) return;
    if (run && (background || CaptureSettingsChanged(run->Options(), options))) {
      permission.Cancel();
      if (background) {
        run->End(CaptureError{CaptureErrorCode::Interrupted, "The application entered the background."});
      } else {
        run->End();
      }
      ClearInputLevel();
      if (producer) {
        status = CaptureStatus{.state = SessionState::Stopping, .format = status->format};
        if (auto stop = std::exchange(stop_capture, {})) stop();
        return;
      }
      run.reset();
    }
    if (!options.active || background) {
      status = CaptureStatus{.state = options.active ? SessionState::Suspended : SessionState::Stopped};
      return;
    }
    if (run) {
      UpdateInputLevelMonitoring();
      return;
    }
    if (const auto rate = options.sample_rate;
        rate && (*rate == 0 || *rate > std::numeric_limits<std::int32_t>::max())) {
      failure = CaptureError{CaptureErrorCode::Unavailable, "The requested sample rate is out of range."};
    } else if (const auto layout = options.channel_layout;
               layout && *layout != media::AudioChannelLayout::Mono && *layout != media::AudioChannelLayout::Stereo) {
      failure = CaptureError{CaptureErrorCode::Unavailable, "The requested channel layout is unsupported."};
    } else if (const auto& id = options.device_id; id && (id->empty() || id->find('\0') != std::string::npos)) {
      failure = CaptureError{CaptureErrorCode::Unavailable, "The requested microphone ID is invalid."};
    }
    if (!backend.start && !failure)
      failure = CaptureError{CaptureErrorCode::Unavailable, "Microphone capture is unavailable on this platform."};
    if (failure) {
      status = CaptureStatus{.state = SessionState::Failed, .error = failure};
      return;
    }
    Begin();
  }

  void Begin() {
    const auto id = ++generation;
    auto weak = weak_from_this();
    run = std::make_shared<CaptureRun>([weak, scope = tasks, id](media::AudioFormat format) {
      scope.Post([weak, id, format] {
        if (auto self = weak.lock(); self && self->generation == id && self->run &&
            self->status->state == SessionState::Starting && !self->run->StopToken().stop_requested()) {
          self->status = CaptureStatus{
              .state = SessionState::Running, .format = format, .recording_paused = self->run->RecordingPaused()};
        }
      });
    }, options);
    status = CaptureStatus{.state = SessionState::Starting};
    UpdateInputLevelMonitoring();
    if (!backend.use_sdk_permission) {
      StartProducer();
      return;
    }
    permission = tasks.Launch([weak, app = application, id]() -> Task<void> {
      try {
        const auto access = co_await app.CheckPermissionAsync(Permission::Microphone);
        auto self = weak.lock();
        if (!self || !self->run || self->generation != id) co_return;
        if (access != PermissionStatus::Granted) {
          self->FailStart(PermissionError(access));
          co_return;
        }
        self->StartProducer();
      } catch (const std::exception& error) {
        if (auto self = weak.lock(); self && self->run && self->generation == id)
          self->FailStart({CaptureErrorCode::CaptureFailed, error.what()});
      }
    });
  }

  void StartProducer() {
    try {
      stop_capture = backend.start(run, [weak = weak_from_this(), current = run, scope = tasks](
                                           std::optional<CaptureError> error) {
        if (!error && !current->StopToken().stop_requested())
          error = CaptureError{CaptureErrorCode::Interrupted, "Microphone capture ended unexpectedly."};
        current->End(std::move(error));
        scope.Post([weak, current] {
          if (auto self = weak.lock(); self && self->run == current) {
            self->ClearInputLevel();
            if (self->status->state == SessionState::Stopping) {
              self->status = CaptureStatus{};
            } else {
              self->failure = current->Error();
            }
            self->stop_capture = {};
            self->producer = false;
            self->run.reset();
            self->Reconcile();
          }
        });
      });
      producer = true;
    } catch (const std::exception& error) {
      FailStart({CaptureErrorCode::CaptureFailed, error.what()});
    }
  }

  void FailStart(CaptureError error) {
    run->End(error);
    ClearInputLevel();
    run.reset();
    failure = std::move(error);
    Reconcile();
  }

  void UpdateOptions(CaptureOptions value) {
    if (closed) return;
    if (CaptureSettingsChanged(options, value)) failure.reset();
    options = std::move(value);
    Reconcile();
  }

  void UpdateLifecycle(ApplicationLifecycleState state) {
    if (closed) return;
    const bool value = state == ApplicationLifecycleState::Background;
    if (background && !value) failure.reset();
    background = value;
    Reconcile();
  }

  void Close() {
    closed = true;
    permission.Cancel();
    ClearInputLevel();
    if (run) {
      run->End(CaptureError{CaptureErrorCode::Interrupted, "The owning composition closed audio capture."});
      run.reset();
    }
    if (auto stop = std::exchange(stop_capture, {})) stop();
    status = CaptureStatus{.state = SessionState::Closed};
  }

  void ClearInputLevel() {
    input_level_updates.Cancel();
    input_level_updates = {};
    input_level = std::nullopt;
  }

  void UpdateInputLevelMonitoring() {
    if (!options.monitor_input_level) {
      run->SetInputLevelMonitoring(false);
      ClearInputLevel();
      return;
    }
    if (!run->SetInputLevelMonitoring(true)) return;
    input_level_updates = tasks.Launch([weak = weak_from_this(), current = run]() -> Task<void> {
      for (;;) {
        co_await Delay(std::chrono::milliseconds{50});
        const auto self = weak.lock();
        if (!self || self->run != current) co_return;
        if (current->StopToken().stop_requested()) {
          self->input_level = std::nullopt;
          co_return;
        }
        if (auto level = current->TakeInputLevel()) self->input_level = std::move(level);
      }
    });
  }

  CaptureResult<AudioReader> OpenReader(std::function<void()> recording_released = {}) const {
    if (!run || (status->state != SessionState::Starting && status->state != SessionState::Running))
      return CaptureResult<AudioReader>::Failure({CaptureErrorCode::NotReady, "The microphone is not running."});
    return run->OpenReader(std::move(recording_released));
  }

  void RefreshRecordingStatus() {
    const bool paused = run && run->RecordingPaused();
    if (status->recording_paused == paused) return;
    CaptureStatus next = status;
    next.recording_paused = paused;
    status = std::move(next);
  }

  void SetRecordingPaused(bool paused) {
    if (!run) return;
    run->SetRecordingPaused(paused);
    RefreshRecordingStatus();
  }

  static Task<RecordingResult> Record(std::shared_ptr<Session> session, RecordingDestination destination,
                                      media::CodecId codec, media::ContainerFormat container, std::uint32_t bitrate) {
    auto reader = session->OpenReader([weak = session->weak_from_this(), scope = session->tasks] {
      scope.Post([weak] {
        if (auto self = weak.lock()) self->RefreshRecordingStatus();
      });
    });
    if (!reader.Succeeded()) co_return RecordingResult::Failure(std::move(reader).Error());
    auto result = co_await RecordAudio(std::move(reader).Value(), std::move(destination), codec, container, bitrate);
    session->RefreshRecordingStatus();
    co_return result;
  }
};

} // namespace huxerui::audio::detail

namespace huxerui::audio {

void Install(ApplicationContext& context) {
#if defined(__ANDROID__) && defined(HUXERUI_AUDIO_CAPTURE_AVAILABLE)
  const auto factory = [](Runtime& runtime) -> detail::CaptureBackend {
    return {.start = &detail::StartCaptureWorker, .enumerate = detail::MakeAndroidInputEnumerator(runtime)};
  };
#else
  const auto factory = [](Runtime&) -> detail::CaptureBackend {
#if defined(HUXERUI_AUDIO_CAPTURE_AVAILABLE) && defined(__EMSCRIPTEN__)
    return {.start = &detail::StartWebCapture, .use_sdk_permission = false,
            .enumerate = &detail::EnumerateWebInputDevices};
#elif defined(HUXERUI_AUDIO_CAPTURE_AVAILABLE)
    return {.start = &detail::StartCaptureWorker,
#ifdef __linux__
            .use_sdk_permission = false,
#endif
            .enumerate = [] { return RunWorker(&detail::EnumeratePlatformInputDevices); }};
#else
    return {};
#endif
  };
#endif
  context.RegisterPlatformModule<detail::CaptureBackend>(detail::capture_module, factory);
}

Task<CaptureResult<std::vector<InputDevice>>> EnumerateInputDevicesAsync() {
  using Result = detail::InputDevicesResult;
  try {
    auto backend = OpenPlatformModule<detail::CaptureBackend>(detail::capture_module);
    if (!backend.enumerate)
      co_return Result::Failure({CaptureErrorCode::Unavailable, "Microphone discovery is unavailable on this platform."});
    if (backend.use_sdk_permission) {
      const auto permission = co_await UseApplication().CheckPermissionAsync(Permission::Microphone);
      if (permission != PermissionStatus::Granted) co_return Result::Failure(detail::PermissionError(permission));
    }
    co_return co_await backend.enumerate();
  } catch (const std::exception& error) {
    co_return Result::Failure({CaptureErrorCode::CaptureFailed, error.what()});
  }
}

AudioCaptureSession::AudioCaptureSession(std::shared_ptr<detail::Session> data) : data_(std::move(data)) {}

CaptureStatus AudioCaptureSession::Status() const { return data_->status; }

std::optional<AudioInputLevel> AudioCaptureSession::InputLevel() const { return data_->input_level; }

void AudioCaptureSession::Retry() const {
  if (data_->status->state != SessionState::Failed) return;
  data_->failure.reset();
  data_->Reconcile();
}

CaptureResult<AudioReader> AudioCaptureSession::OpenReader() const { return data_->OpenReader(); }

Task<CaptureResult<RecordingSummary>> AudioCaptureSession::RecordAsync(
    File destination, media::CodecId codec, media::ContainerFormat container, std::uint32_t bitrate) const {
  return detail::Session::Record(data_, std::move(destination), codec, container, bitrate);
}

Task<CaptureResult<RecordingSummary>> AudioCaptureSession::RecordAsync(
    RandomAccessStream destination, media::CodecId codec, media::ContainerFormat container,
    std::uint32_t bitrate) const {
  return detail::Session::Record(data_, std::move(destination), codec, container, bitrate);
}

Task<CaptureResult<RecordingSummary>> AudioCaptureSession::RecordAsync(
    OutputStream destination, media::CodecId codec, media::ContainerFormat container, std::uint32_t bitrate) const {
  return detail::Session::Record(data_, std::move(destination), codec, container, bitrate);
}

void AudioCaptureSession::PauseRecording() const { data_->SetRecordingPaused(true); }

void AudioCaptureSession::ResumeRecording() const { data_->SetRecordingPaused(false); }

AudioCaptureSession UseAudioCapture(CaptureOptions options, const std::source_location& location) {
  auto status = UseState(CaptureStatus{}, location);
  auto input_level = UseState(std::optional<AudioInputLevel>{}, location);
  auto tasks = UseTaskScope();
  auto app = UseApplication();
  auto retained = UseState(std::make_shared<detail::Session>(status, input_level, tasks, app), location);
  std::shared_ptr<detail::Session> data = retained;
  const bool background = app.LifecycleState() == ApplicationLifecycleState::Background;
  Lifecycle([data, background] {
    data->background = background;
    data->backend = OpenPlatformModule<detail::CaptureBackend>(detail::capture_module);
    return [data] { data->Close(); };
  }, data);
  app.OnLifecycleChanged([data](ApplicationLifecycleState state) { data->UpdateLifecycle(state); }, data);
  Lifecycle([data, options] {
    data->UpdateOptions(options);
    return [] {};
  }, data, options);
  return AudioCaptureSession(std::move(data));
}

} // namespace huxerui::audio
