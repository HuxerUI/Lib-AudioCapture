#include <huxerui/huxerui.h>
#include <huxerui/audiocapture.h>

#include <chrono>
#include <optional>

using namespace huxerui;

namespace {

const File& RecordingDirectory(const ApplicationHandle& app) {
#ifdef __EMSCRIPTEN__
  return app.Directories().temporary_directory;
#else
  return app.Directories().data_directory;
#endif
}

#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
void StartRecording(ApplicationHandle, TaskScope, State<bool>, State<bool> active, State<std::string> message) {
  message = std::string{"Starting microphone"};
  active = true;
}
#else
void StartRecording(ApplicationHandle app, TaskScope tasks, State<bool> requesting,
                    State<bool> active, State<std::string> message) {
  requesting = true;
  tasks.Launch([app, requesting, active, message]() -> Task<void> {
    auto permission = co_await app.CheckPermissionAsync(Permission::Microphone);
    if (permission != PermissionStatus::Granted)
      permission = co_await app.RequestPermissionAsync(Permission::Microphone);
    requesting = false;
    if (permission == PermissionStatus::Granted) {
      message = std::string{"Starting microphone"};
      active = true;
    } else {
      message = std::string{"Microphone access was not granted"};
    }
  });
}
#endif

} // namespace

[[huxerui::composable]]
View Recorder() {
  auto app = UseApplication();
  auto tasks = UseTaskScope();
  auto picker = UseService<FilePicker>();
  auto active = UseState(false);
  auto requesting = UseState(false);
  auto recording = UseState(false);
  auto saving = UseState(false);
  auto last_file = UseState(std::optional<File>{});
  auto message = UseState(std::string{"Ready to record"});
  const bool wants_recording = active;
  const auto session = audio::UseAudioCapture({.active = wants_recording});
  const auto status = session.Status();

  Lifecycle([session, status, wants_recording, app, tasks, active, recording, last_file, message] {
    if (wants_recording && !static_cast<bool>(recording)) {
      if (status.error) {
        message = status.error->message;
        active = false;
      } else if (status.state == audio::SessionState::Starting || status.state == audio::SessionState::Running) {
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        auto file = RecordingDirectory(app).Child("recording-" + std::to_string(stamp) + ".wav");
        recording = true;
        tasks.Launch([session, file, active, recording, last_file, message]() -> Task<void> {
          auto result = co_await session.RecordAsync(file, media::CodecId::PcmS16Le, media::ContainerFormat::Wave);
          active = false;
          recording = false;
          if (result.Succeeded()) {
            last_file = file;
            message = "Recorded " + std::to_string(result.Value().duration.ticks) + " samples per channel";
          } else {
            message = result.Error().message;
          }
        });
      }
    }
    return [] {};
  }, wants_recording, status);

  const bool is_recording = recording;
  const bool is_paused = status.recording_paused;
  const auto recording_message = is_paused ? "Recording paused" : "Recording";
  const std::optional<File> completed = last_file;
  return Column {
    Text("Microphone recording"),
    Text(status.state == audio::SessionState::Running ? recording_message :
         (status.state == audio::SessionState::Stopping ? "Finishing recording" : static_cast<std::string>(message))),
    Button("Start recording").OnClick([app, tasks, requesting, active, message] {
      StartRecording(app, tasks, requesting, active, message);
    }).With(Enabled(!wants_recording && !requesting && !is_recording && status.state != audio::SessionState::Stopping)),
    Button(is_paused ? "Resume recording" : "Pause recording").OnClick([session] {
      if (session.Status().recording_paused) {
        session.ResumeRecording();
      } else {
        session.PauseRecording();
      }
    }).With(Enabled(wants_recording && is_recording && status.state == audio::SessionState::Running)),
    Button("Stop recording").OnClick([active] { active = false; }).With(Enabled(wants_recording)),
    Button("Export WAVE").OnClick([tasks, picker, completed, saving, message] {
      if (!completed) return;
      saving = true;
      tasks.Launch([picker, file = *completed, saving, message]() -> Task<void> {
        SaveFileOptions options{
            .suggested_name = file.Name(),
            .filter = {.name = "WAVE audio", .extensions = {"wav"}, .content_types = {"audio/wav"}},
        };
        const bool saved = co_await picker->SaveFileAsync(file, options);
        saving = false;
        message = saved ? std::string{"WAVE file exported"} : std::string{"WAVE file was not exported"};
      });
    }).With(Enabled(completed.has_value() && picker->CanSaveFiles() && !saving)),
  }.With(Padding{24.0F});
}

View App() {
  return Recorder();
}

const Application application{
    App,
    {
        .window = {
            .title = "Lib-AudioCapture Preview",
        },
        .application_hooks = {huxerui::audio::Install},
    }
};
