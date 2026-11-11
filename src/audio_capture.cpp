#include "capture_run.h"

#include <thread>
#include <utility>

#include <huxerui/lifecycle.h>
#include <huxerui/state.h>
#include <huxerui/system.h>

namespace huxerui::audio::detail {
namespace {

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
  TaskScope tasks;
  ApplicationHandle application;
  CaptureBackend backend;
  std::shared_ptr<CaptureRun> run;
  std::function<void()> stop_capture;
  TaskHandle permission;
  std::optional<CaptureError> failure;
  std::uint64_t generation = 0;
  bool active = false;
  bool background = false;
  bool producer = false;
  bool closed = false;

  Session(State<CaptureStatus> status, TaskScope tasks, ApplicationHandle app)
      : status(status), tasks(tasks), application(app) {}

  void Reconcile() {
    if (closed || status->state == SessionState::Stopping) return;
    if (!active || background) {
      if (run) {
        permission.Cancel();
        if (background) {
          run->End(CaptureError{CaptureErrorCode::Interrupted, "The application entered the background."});
        } else {
          run->End();
        }
        if (producer) {
          status = CaptureStatus{.state = SessionState::Stopping, .format = status->format};
          if (auto stop = std::exchange(stop_capture, {})) stop();
          return;
        }
        run.reset();
      }
      status = CaptureStatus{.state = active && background ? SessionState::Suspended : SessionState::Stopped};
      return;
    }
    if (run) return;
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
          self->status = CaptureStatus{.state = SessionState::Running, .format = format};
        }
      });
    });
    status = CaptureStatus{.state = SessionState::Starting};
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
    run.reset();
    failure = std::move(error);
    Reconcile();
  }

  void UpdateIntent(bool value) {
    if (closed) return;
    if (active != value) failure.reset();
    active = value;
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
    if (run) {
      run->End(CaptureError{CaptureErrorCode::Interrupted, "The owning composition closed audio capture."});
      run.reset();
    }
    if (auto stop = std::exchange(stop_capture, {})) stop();
    status = CaptureStatus{.state = SessionState::Closed};
  }

  CaptureResult<AudioReader> OpenReader() const {
    if (!run || (status->state != SessionState::Starting && status->state != SessionState::Running))
      return CaptureResult<AudioReader>::Failure({CaptureErrorCode::NotReady, "The microphone is not running."});
    return run->OpenReader();
  }
};

} // namespace huxerui::audio::detail

namespace huxerui::audio {

void Install(ApplicationContext& context) {
  context.RegisterPlatformModule<detail::CaptureBackend>(detail::capture_module, [](Runtime&) -> detail::CaptureBackend {
#if defined(__EMSCRIPTEN__) && defined(HUXERUI_AUDIO_CAPTURE_AVAILABLE)
    return {.start = &detail::StartWebCapture, .use_sdk_permission = false};
#elif defined(__linux__) && !defined(__ANDROID__) && defined(HUXERUI_AUDIO_CAPTURE_AVAILABLE)
    return {.start = &detail::StartCaptureWorker, .use_sdk_permission = false};
#elif defined(HUXERUI_AUDIO_CAPTURE_AVAILABLE)
    return {.start = &detail::StartCaptureWorker};
#else
    return {};
#endif
  });
}

AudioCaptureSession::AudioCaptureSession(std::shared_ptr<detail::Session> data) : data_(std::move(data)) {}

CaptureStatus AudioCaptureSession::Status() const { return data_->status; }

void AudioCaptureSession::Retry() const {
  if (data_->status->state != SessionState::Failed) return;
  data_->failure.reset();
  data_->Reconcile();
}

CaptureResult<AudioReader> AudioCaptureSession::OpenReader() const { return data_->OpenReader(); }

AudioCaptureSession UseAudioCapture(CaptureOptions options, const std::source_location& location) {
  auto status = UseState(CaptureStatus{}, location);
  auto tasks = UseTaskScope();
  auto app = UseApplication();
  auto retained = UseState(std::make_shared<detail::Session>(status, tasks, app), location);
  std::shared_ptr<detail::Session> data = retained;
  const bool background = app.LifecycleState() == ApplicationLifecycleState::Background;
  Lifecycle([data, background] {
    data->background = background;
    data->backend = OpenPlatformModule<detail::CaptureBackend>(detail::capture_module);
    return [data] { data->Close(); };
  }, data);
  app.OnLifecycleChanged([data](ApplicationLifecycleState state) { data->UpdateLifecycle(state); }, data);
  Lifecycle([data, active = options.active] {
    data->UpdateIntent(active);
    return [] {};
  }, data, options.active);
  return AudioCaptureSession(std::move(data));
}

} // namespace huxerui::audio
