#include <huxerui/huxerui.h>
#include <huxerui/media_player.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>

#include "voice_effects.h"

using namespace huxerui;
using namespace std::chrono_literals;

namespace {

using voice_studio::VoicePreset;

const auto background = Color::Rgb(10, 15, 23);
const auto surface = Color::Rgb(19, 27, 39);
const auto control = Color::Rgb(27, 39, 54);
const auto outline = Color::Rgb(45, 60, 77);
const auto foreground = Color::Rgb(237, 245, 252);
const auto secondary = Color::Rgb(159, 179, 199);
const auto accent = Color::Rgb(107, 234, 204);
const auto recording_color = Color::Rgb(255, 129, 151);
const auto paused_color = Color::Rgb(253, 200, 125);

struct PresetInfo {
  VoicePreset preset;
  const char* name;
  const char* description;
};

constexpr std::array presets{
    PresetInfo{VoicePreset::Original, "Original", "Unprocessed microphone audio."},
    PresetInfo{VoicePreset::Deep, "Deep", "Lower pitch and formants for a fuller voice."},
    PresetInfo{VoicePreset::Chipmunk, "Chipmunk", "Higher pitch and formants for a bright, cartoon voice."},
    PresetInfo{VoicePreset::Robot, "Robot", "Ring modulation adds a metallic buzz."},
    PresetInfo{VoicePreset::Radio, "Radio", "Narrow-band filtering and soft clipping simulate a walkie-talkie."},
    PresetInfo{VoicePreset::SlowMotion, "Slow Motion", "0.8x speed with pitch preserved. The take is 25% longer."},
};

struct Take {
  File file;
  VoicePreset preset;
  media::AudioFormat format;
  media::MediaTimestamp duration;

  bool operator==(const Take&) const = default;
};

struct Meter {
  std::array<float, 64> history{};
  std::size_t next = 0;
  std::optional<audio::AudioInputLevel::Channel> level;

  bool operator==(const Meter&) const = default;
};

Text Label(StringVariant text, float size = 14.0F, Color color = foreground,
           FontWeight weight = FontWeight::Regular) {
  return Text(std::move(text)).Style({Font::System(size).WithWeight(weight), color});
}

Text MonoLabel(StringVariant text, float size, Color color = secondary) {
  return Text(std::move(text)).Style({Font::Monospace(size), color});
}

std::string ClockLabel(double seconds) {
  const auto tenths = static_cast<std::int64_t>(std::max(0.0, seconds) * 10.0);
  return std::format("{:02}:{:02}.{}", tenths / 600, tenths / 10 % 60, tenths % 10);
}

std::string FormatLabel(const media::AudioFormat& format) {
  return std::format("{:g} kHz / {}", format.sample_rate / 1000.0, format.ChannelCount() == 1 ? "Mono" : "Stereo");
}

float Dbfs(float level) { return 20.0F * std::log10(level); }

std::string LevelLabel(float level) {
  return level > 0.0F ? std::format("{:.1f} dBFS", Dbfs(level)) : "-inf dBFS";
}

ThemeSpec StudioColors() {
  auto spec = FlatDarkThemeSpec();
  spec.colors.primary = accent;
  spec.colors.on_primary = background;
  spec.colors.primary_container = Color::Rgb(25, 60, 59);
  spec.colors.on_primary_container = accent;
  spec.colors.background = background;
  spec.colors.surface = surface;
  spec.colors.surface_container = surface;
  spec.colors.surface_container_high = control;
  spec.colors.on_surface = foreground;
  spec.colors.on_surface_variant = secondary;
  spec.colors.outline = outline;
  return spec;
}

ButtonStyle ActionStyle(bool primary) {
  ButtonStyle style;
  style.background = primary ? accent : control;
  style.label_style = {Font::System(14.0F).WithWeight(FontWeight::SemiBold), primary ? background : foreground};
  style.disabled_background = Color::Rgb(24, 33, 46);
  style.disabled_label = Color::Rgb(107, 126, 145);
  style.minimum_height = 48.0F;
  style.corner_radii = CornerRadii{11.0F};
  return style;
}

ThemeDefinition StudioControls() {
  ThemeDefinition theme;
  theme.Set(ActionStyle(true));
  ChipStyle chip;
  chip.background = control;
  chip.selected_background = Color::Rgb(25, 58, 57);
  chip.label_style = {Font::System(14.0F).WithWeight(FontWeight::SemiBold), foreground};
  chip.selected_label = accent;
  chip.border = Border{outline};
  chip.selected_border = Border{accent};
  chip.disabled_background = surface;
  chip.disabled_selected_background = Color::Rgb(25, 53, 53);
  chip.disabled_label = secondary;
  chip.disabled_selected_label = accent;
  chip.disabled_border = Border{outline};
  chip.disabled_selected_border = Border{Color::Rgb(70, 144, 134)};
  chip.minimum_height = 54.0F;
  chip.corner_radii = CornerRadii{11.0F};
  theme.Set(chip);

  SliderStyle slider;
  slider.active_track = accent;
  slider.thumb = accent;
  slider.inactive_track = outline;
  slider.height = 36.0F;
  theme.Set(slider);
  return theme;
}

View SecondaryAction(View action) {
  ThemeDefinition theme;
  theme.Set(ActionStyle(false));
  return Theme(theme, std::move(action));
}

View Panel(View content) {
  return std::move(content).With(Padding{20.0F}, Background{surface}, Border{outline}, CornerRadius(18.0F));
}

const File& RecordingDirectory(const ApplicationHandle& app) {
#ifdef __EMSCRIPTEN__
  return app.Directories().temporary_directory;
#else
  return app.Directories().data_directory;
#endif
}

#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
void StartRecording(ApplicationHandle, TaskScope, State<bool>, State<bool> active, State<std::string>) {
  active = true;
}
#else
void StartRecording(ApplicationHandle app, TaskScope tasks, State<bool> authorizing,
                    State<bool> active, State<std::string> error) {
  authorizing = true;
  tasks.Launch([app, authorizing, active, error]() -> Task<void> {
    auto permission = co_await app.CheckPermissionAsync(Permission::Microphone);
    if (permission != PermissionStatus::Granted)
      permission = co_await app.RequestPermissionAsync(Permission::Microphone);
    authorizing = false;
    if (permission == PermissionStatus::Granted) {
      active = true;
    } else {
      error = "Microphone access is required to record.";
    }
  });
}
#endif

View LevelGraph(const Meter& meter, bool wide) {
  return Canvas([meter](PaintContext& paint, Size size) {
    constexpr float inset = 14.0F;
    const float width = size.width - 2 * inset;
    const float height = size.height - 2 * inset;
    if (width <= 0.0F || height <= 0.0F) return;
    for (const float fraction : {0.1F, 0.3F, 0.5F, 0.7F, 0.9F}) {
      const auto y = inset + fraction * height;
      paint.DrawLine({inset, y}, {size.width - inset, y}, outline, StrokeStyle{.width = 1.0F});
    }

    const auto step = width / meter.history.size();
    for (std::size_t index = 0; index < meter.history.size(); ++index) {
      const auto level = meter.history[(meter.next + index) % meter.history.size()];
      const auto bar_height = std::max(2.0F, level * height);
      const auto opacity = 0.3F + 0.7F * static_cast<float>(index + 1) / meter.history.size();
      paint.DrawRect({inset + step * index, (size.height - bar_height) / 2.0F,
                      step * 0.64F, bar_height},
                     Color::Rgb(107, 234, 204, opacity), CornerRadii{2.0F});
    }
  }).With(Frame{.height = wide ? 168.0F : 124.0F}, Background{background}, CornerRadius(12.0F));
}

[[huxerui::composable]]
View InputMonitor(audio::AudioCaptureSession session, bool live, bool wide, State<double> elapsed) {
  const auto tasks = UseTaskScope();
  auto meter = UseState(Meter{});
  Lifecycle([tasks, session, live, meter]() -> std::function<void()> {
    if (!live) return [] {};
    meter = Meter{};
    const auto sampling = tasks.Launch([session, meter]() -> Task<void> {
      Meter next;
      for (;;) {
        if (const auto input = session.InputLevel()) {
          audio::AudioInputLevel::Channel level;
          for (std::uint32_t channel = 0; channel < input->channel_count; ++channel) {
            level.rms = std::max(level.rms, input->channels[channel].rms);
            level.peak = std::max(level.peak, input->channels[channel].peak);
          }
          next.level = level;
          next.history[next.next] = level.rms > 0.0F
              ? std::clamp((Dbfs(level.rms) + 60.0F) / 60.0F, 0.0F, 1.0F)
              : 0.0F;
          next.next = (next.next + 1) % next.history.size();
          meter = next;
        }
        co_await Delay(50ms);
      }
    });
    return [sampling] { sampling.Cancel(); };
  }, live);

  const Meter snapshot = meter;
  const auto rms = live && snapshot.level ? LevelLabel(snapshot.level->rms) : "-- dBFS";
  const auto peak = live && snapshot.level ? LevelLabel(snapshot.level->peak) : "-- dBFS";
  return Column {
    Row {
      MonoLabel(ClockLabel(elapsed), wide ? 40.0F : 32.0F, foreground),
      Spacer(),
      Column {
        MonoLabel(rms, 14.0F, accent),
        Label("Peak " + peak, 12.0F, secondary),
      }.With(Spacing(4.0F)),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)),
    LevelGraph(snapshot, wide),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View EffectPicker(State<VoicePreset> selected, bool busy) {
  const VoicePreset current = selected;
  const auto card = [selected, current, busy](std::size_t index) {
    const auto preset = presets[index];
    return Chip(preset.name, current == preset.preset)
        .OnChanged([selected, preset](bool) { selected = preset.preset; })
        .With(Grow(), Enabled(!busy));
  };
  return Panel(Column {
    Label("Voice effects", 19.0F, foreground, FontWeight::SemiBold),
    Row { card(0), card(1) }.With(Spacing(10.0F)),
    Row { card(2), card(3) }.With(Spacing(10.0F)),
    Row { card(4), card(5) }.With(Spacing(10.0F)),
    Label(presets[static_cast<std::size_t>(current)].description, 13.0F, secondary).With(Frame{.min_height = 36.0F}),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]]
View LatestTake(Take take, media::MediaPlayer player, bool busy) {
  const auto tasks = UseTaskScope();
  const auto picker = UseService<FilePicker>();
  auto saving = UseState(false);
  auto notice = UseState(std::string{});
  auto scrub = UseState(std::optional<float>{});
  const auto state = player.Snapshot();
  player.OnError([notice](const media::MediaError& error) { notice = error.message; });
  const auto duration = static_cast<double>(take.duration.ticks) / take.duration.timescale;
  const auto position = scrub->value_or(static_cast<float>(state.progress.position.count()));
  const auto play = state.play_when_ready;
  const auto loading = state.status == media::MediaPlayerStatus::Loading;
  const auto can_seek = state.seekability == media::MediaSeekability::Seekable;
  const bool exporting = saving;
  const std::string& notice_text = notice;

  return Panel(Column {
    Row {
      Label("Latest take", 19.0F, foreground, FontWeight::SemiBold),
      Spacer(),
      Label(presets[static_cast<std::size_t>(take.preset)].name, 13.0F, accent, FontWeight::SemiBold),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    Label("WAVE / " + FormatLabel(take.format), 13.0F, secondary),
    Slider(std::clamp(position, 0.0F, static_cast<float>(duration))).Range(0.0F, static_cast<float>(duration))
        .OnChanged([scrub](float value) { scrub = value; })
        .OnCommitted([player, scrub](float value) {
          scrub = std::nullopt;
          player.SeekTo(media::MediaTime{value});
        })
        .OnCanceled([scrub] { scrub = std::nullopt; })
        .With(Enabled(!busy && can_seek), Semantics{.label = "Playback position"}),
    Row {
      MonoLabel(ClockLabel(position), 12.0F),
      Spacer(),
      MonoLabel(ClockLabel(duration), 12.0F),
    },
    Row {
      Button(loading ? "Loading" : play ? "Pause playback" : "Play").OnClick([player, take, notice, play] {
        notice = std::string{};
        if (play) {
          player.Pause();
        } else {
          const auto status = player.Snapshot().status;
          if (status == media::MediaPlayerStatus::Empty || status == media::MediaPlayerStatus::Failed) {
            player.Load(media::MediaSource(take.file));
          }
          if (!player.Play()) notice = "Playback is unavailable.";
        }
      }).With(Enabled(!busy && !loading)),
      SecondaryAction(Button(exporting ? "Exporting" : "Export WAVE").OnClick([tasks, picker, take, saving, notice] {
        saving = true;
        notice = std::string{};
        tasks.Launch([picker, file = take.file, saving, notice]() -> Task<void> {
          const SaveFileOptions options{
              .suggested_name = file.Name(),
              .filter = {.name = "WAVE audio", .extensions = {"wav"}, .content_types = {"audio/wav"}},
          };
          const bool saved = co_await picker->SaveFileAsync(file, options);
          saving = false;
          notice = saved ? "WAVE exported." : "Export canceled.";
        });
      }).With(Enabled(picker->CanSaveFiles() && !exporting))),
    }.With(Spacing(10.0F)),
    notice_text.empty() ? View{} : Label(notice_text, 13.0F, secondary),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]]
View VoiceStudio() {
  const auto app = UseApplication();
  const auto tasks = UseTaskScope();
  const auto player = media::UseMediaPlayer();
  const bool wide = UseViewportClass() == ViewportClass::Expanded;
  auto preset = UseState(VoicePreset::Original);
  auto active = UseState(false);
  auto authorizing = UseState(false);
  auto recording = UseState(false);
  auto elapsed = UseState(0.0);
  auto latest = UseState(std::optional<Take>{});
  auto error = UseState(std::string{});
  const VoicePreset selected = preset;
  const bool wants_recording = active;
  const auto session = audio::UseAudioCapture({
      .active = wants_recording,
      .monitor_input_level = true,
      .pcm_processor = voice_studio::ProcessorForPreset(selected),
  });
  const auto status = session.Status();
  const bool live = status.state == audio::SessionState::Running;
  const bool paused = status.recording_paused;
  const bool finishing = !wants_recording && (recording || status.state == audio::SessionState::Stopping);
  const bool busy = authorizing || wants_recording || finishing;
  const bool can_pause = wants_recording && live && recording;
  const bool ticking = can_pause && !paused;

  Lifecycle([session, status, wants_recording, selected, app, tasks, player, active, recording, latest, error] {
    if (wants_recording && !recording) {
      if (status.error) {
        error = status.error->message;
        active = false;
      } else if (status.state == audio::SessionState::Starting || status.state == audio::SessionState::Running) {
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        auto file = RecordingDirectory(app).Child("voice-" + std::to_string(stamp) + ".wav");
        recording = true;
        tasks.Launch([session, file, selected, player, active, recording, latest, error]() -> Task<void> {
          auto result = co_await session.RecordAsync(file, media::CodecId::PcmS16Le, media::ContainerFormat::Wave);
          active = false;
          recording = false;
          if (result.Succeeded()) {
            const auto& summary = result.Value();
            latest = Take{file, selected, summary.format, summary.duration};
            player.Load(media::MediaSource(file));
          } else {
            error = result.Error().message;
          }
        });
      }
    }
  }, wants_recording, status);

  Lifecycle([tasks, elapsed, busy, ticking]() -> std::function<void()> {
    if (!busy) elapsed = 0.0;
    if (!ticking) return [] {};
    const auto start = std::chrono::steady_clock::now();
    const double previous = elapsed;
    const auto update = [elapsed, start, previous] {
      elapsed = previous + std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    const auto timer = tasks.Launch([update]() -> Task<void> {
      for (;;) {
        co_await Delay(100ms);
        update();
      }
    });
    return [timer, update] {
      timer.Cancel();
      update();
    };
  }, busy, ticking);

  const auto status_text = authorizing ? "Microphone access" : finishing ? "Finishing" : paused ? "Paused"
      : ticking ? "Recording" : status.state == audio::SessionState::Suspended ? "Suspended"
      : wants_recording ? "Starting" : "Ready";
  const std::string& error_text = error;
  auto input = Panel(Column {
    Row {
      Label("Microphone", 19.0F, foreground, FontWeight::SemiBold),
      Spacer(),
      Label(status_text, 13.0F, paused ? paused_color : ticking ? recording_color : secondary, FontWeight::SemiBold),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    InputMonitor(session, live, wide, elapsed),
    status.format ? Label(FormatLabel(*status.format), 12.0F, secondary) : View{},
    Row {
      Button(can_pause ? paused ? "Resume recording" : "Pause recording" : "Record")
          .OnClick([app, tasks, player, session, authorizing, active, error, can_pause] {
            if (can_pause) {
              if (session.Status().recording_paused) session.ResumeRecording();
              else session.PauseRecording();
            } else {
              player.Pause();
              error = std::string{};
              StartRecording(app, tasks, authorizing, active, error);
            }
          }).With(Grow(), Enabled(!busy || can_pause)),
      SecondaryAction(Button("Stop").OnClick([active] { active = false; })
          .With(Enabled(wants_recording))),
    }.With(Spacing(10.0F)),
    error_text.empty() ? View{} : Label(error_text, 13.0F, recording_color),
  }.With(Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch)));

  auto effects = EffectPicker(preset, busy);
  View workspace;
  if (wide) {
    workspace = Row { std::move(input).With(Grow(1.55F)), std::move(effects).With(Grow()) }
        .With(Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch));
  } else {
    workspace = Column { std::move(input), std::move(effects) }
        .With(Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch));
  }
  const std::optional<Take> take = latest;
  return ScrollView(Stack {
    Column {
      Label("Voice Studio", wide ? 36.0F : 30.0F, foreground, FontWeight::Bold),
      std::move(workspace),
      take ? LatestTake(*take, player, busy).Key(take->file.Name()) : View{},
    }.With(Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch), Frame{.max_width = 1180.0F},
           Padding{wide ? 26.0F : 16.0F}),
  }.With(Align(HorizontalAlignment::Center, VerticalAlignment::Start)))
      .With(Background{background}, ScrollBar());
}

} // namespace

View App() {
  return FlatTheme(StudioColors(), Theme(StudioControls(), VoiceStudio()));
}

const Application application{
    App,
    {
        .window = {
            .title = "Voice Studio",
            .initial_size = {1120.0F, 760.0F},
            .minimum_size = Size{360.0F, 520.0F},
        },
        .show_debug_overlay = false,
        .application_hooks = {audio::Install, media::Install},
    }
};
