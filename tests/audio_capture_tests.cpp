#include <huxerui/huxerui.h>
#include <huxerui/media_container.h>
#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "capture_run.h"

using namespace huxerui;
using namespace huxerui::audio;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace {

using audio::detail::CaptureRun;
using audio::detail::mono_48k;

const InputDevice test_microphone{"test-microphone", "Test microphone"};

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T, class E>
T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

void TestTimestampsAndQueue() {
  using audio::detail::SampleTimestamp;
  Check(SampleTimestamp(47520, 48000, 32000000000, 48000) == MediaTimestamp{31990000000, 1000000000},
        "Timestamp must refer to the earlier first sample, not the anchor or delivery time.");
  Check(SampleTimestamp(44101, 44100, 32000000000, 44100) == MediaTimestamp{32000022676, 1000000000},
        "Timestamp must use the actual sample rate.");
  Check(!SampleTimestamp(1, 0, std::numeric_limits<std::int64_t>::max(), 48000),
        "Overflowing clock correlation must be rejected.");

  const auto run = std::make_shared<CaptureRun>(nullptr);
  auto reader = Require(run->OpenReader());
  auto duplicate = run->OpenReader();
  Check(!duplicate.Succeeded() && duplicate.Error().code == CaptureErrorCode::ReaderInUse,
        "A run must have only one PCM consumer.");
  std::array<std::int16_t, 3> samples{14, -22, 81};
  const MediaTimestamp pts{10, 1};
  Check(run->Publish(mono_48k, CaptureClock::CoreMediaHostTime, pts, samples), "Valid PCM was rejected.");
  samples[0] = 0;
  run->End();
  auto block = Require(reader.Read());
  Check(block && block->clock == CaptureClock::CoreMediaHostTime && block->frame.pts == pts && block->frame.Validate(),
        "The delivered block lost its format, time, or clock domain.");
  Check(block->frame.planes.front().AsBytes()[0] == std::byte{14}, "PCM storage was not retained independently.");
  Check(!Require(reader.Read()), "A clean stop must drain before delivering EOF.");
  Check(!run->OpenReader().Succeeded(), "An ended run must not accept another reader.");

  const auto overloaded = std::make_shared<CaptureRun>(nullptr);
  auto slow_reader = Require(overloaded->OpenReader());
  for (std::int64_t ticks = 0; ticks < 1000; ++ticks) {
    if (!overloaded->Publish(mono_48k, CaptureClock::CoreMediaHostTime, {ticks, 16000}, samples)) break;
  }
  Check(overloaded->StopToken().stop_requested(), "The producer must stop on overflow.");
  auto overflow = slow_reader.Read();
  Check(!overflow.Succeeded() && overflow.Error().code == CaptureErrorCode::QueueOverflow,
        "A slow consumer must see an error instead of losing PCM silently.");

  const auto interrupted = std::make_shared<CaptureRun>(nullptr);
  auto pending = Require(interrupted->OpenReader());
  interrupted->End(CaptureError{CaptureErrorCode::Interrupted, "Synthetic interruption."});
  auto failure = pending.Read();
  Check(!failure.Succeeded() && failure.Error().code == CaptureErrorCode::Interrupted,
        "Interruption must not look like a clean EOF.");

  const auto requested_rate = std::make_shared<CaptureRun>(nullptr, CaptureOptions{.sample_rate = 44100});
  Check(!requested_rate->Publish(mono_48k, CaptureClock::Monotonic, pts, samples) &&
        requested_rate->Error()->code == CaptureErrorCode::Unavailable,
        "A run with a required sample rate must reject mismatched PCM.");

  const auto stereo = std::make_shared<CaptureRun>(nullptr, CaptureOptions{.channel_layout = AudioChannelLayout::Stereo});
  Check(!stereo->Publish(mono_48k, CaptureClock::Monotonic, pts, samples) &&
        stereo->Error()->code == CaptureErrorCode::Unavailable,
        "A run with a required channel layout must reject mismatched PCM.");
}

struct Fixture {
  State<bool> mounted;
  State<CaptureOptions> options;
  TaskScope tasks;
  std::optional<AudioCaptureSession> session;
  std::shared_ptr<CaptureRun> run;
  std::atomic<unsigned> openings = 0;
  bool use_sdk_permission = true;
};

Fixture* fixture;

[[huxerui::composable]]
View CaptureOwner() {
  fixture->session = UseAudioCapture(fixture->options);
  return Text("Capture owner");
}

View TestApp() {
  fixture->mounted = UseState(true);
  fixture->options = UseState(CaptureOptions{});
  fixture->tasks = UseTaskScope();
  return fixture->mounted ? CaptureOwner() : Text("Unmounted");
}

void PumpUntil(testing::UiTest& ui, const auto& ready) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!ready() && std::chrono::steady_clock::now() < deadline) {
    ui.Pump();
    std::this_thread::sleep_for(1ms);
  }
  Check(ready(), "Timed out waiting for the audio operation.");
}

void TestAuthorizationAndLifetime(testing::UiTest& ui) {
  Check(fixture->session->Status().state == SessionState::Stopped, "Capture must begin stopped.");
  fixture->options = CaptureOptions{.active = true};
  PumpUntil(ui, [] { return fixture->session->Status().state == SessionState::Failed; });
  Check(fixture->session->Status().error->code == CaptureErrorCode::Unavailable && fixture->openings == 0,
        "The microphone must not open on a host without permission support.");
  fixture->mounted = false;
  ui.Pump();
  Check(fixture->session->Status().state == SessionState::Closed && !fixture->session->OpenReader().Succeeded(),
        "Retaining a handle must not extend its composition lifetime.");
}

void TestCaptureFormat(testing::UiTest& ui) {
  fixture->use_sdk_permission = false;
  fixture->options = CaptureOptions{.active = true, .sample_rate = 0};
  fixture->mounted = true;
  PumpUntil(ui, [] { return fixture->session->Status().state == SessionState::Failed; });
  Check(fixture->session->Status().error->code == CaptureErrorCode::Unavailable && fixture->openings == 0,
        "An invalid sample rate must fail before opening the microphone.");

  fixture->options = CaptureOptions{.active = true, .channel_layout = static_cast<AudioChannelLayout>(-1)};
  ui.Pump();
  Check(fixture->session->Status().state == SessionState::Failed && fixture->openings == 0,
        "An invalid channel layout must fail before opening the microphone.");

  fixture->options = CaptureOptions{.active = true, .sample_rate = 44100};
  PumpUntil(ui, [] { return fixture->session->Status().state == SessionState::Running; });
  auto format = mono_48k;
  format.sample_rate = 44100;
  Check(fixture->session->Status().format == format && fixture->openings == 1,
        "The session must report the requested PCM sample rate.");
  auto reader = Require(fixture->session->OpenReader());
  const std::array<std::int16_t, 1> pcm{23};
  Check(fixture->run->Publish(format, CaptureClock::Monotonic, {2, 1}, pcm), "The first run rejected valid PCM.");

  fixture->options = CaptureOptions{.active = true, .sample_rate = 16000};
  PumpUntil(ui, [] {
    const auto status = fixture->session->Status();
    return status.state == SessionState::Running && status.format->sample_rate == 16000;
  });
  const auto block = Require(reader.Read());
  Check(fixture->openings == 2 && block && block->frame.format == format && !Require(reader.Read()),
        "Changing the sample rate must drain the first run and start a new run.");

  auto mono_reader = Require(fixture->session->OpenReader());
  format.sample_rate = 16000;
  Check(fixture->run->Publish(format, CaptureClock::Monotonic, {2, 1}, pcm), "The mono run rejected valid PCM.");
  fixture->options = CaptureOptions{.active = true, .sample_rate = 16000, .channel_layout = AudioChannelLayout::Stereo};
  PumpUntil(ui, [] {
    const auto status = fixture->session->Status();
    return status.state == SessionState::Running && status.format->channel_layout == AudioChannelLayout::Stereo;
  });
  const auto mono_block = Require(mono_reader.Read());
  Check(fixture->openings == 3 && mono_block && mono_block->frame.format == format && !Require(mono_reader.Read()),
        "Changing only the channel layout must drain the mono run and start a stereo run.");
  format.channel_layout = AudioChannelLayout::Stereo;
  auto stereo_reader = Require(fixture->session->OpenReader());
  const std::array<std::int16_t, 2> stereo_pcm{23, 51};
  Check(fixture->run->Publish(format, CaptureClock::Monotonic, {2, 1}, stereo_pcm), "The stereo run rejected valid PCM.");
  const auto stereo_block = Require(stereo_reader.Read());
  Check(stereo_block && stereo_block->frame.format == format && stereo_block->frame.sample_count == 1 &&
        std::ranges::equal(stereo_block->frame.planes.front().AsBytes(), std::as_bytes(std::span(stereo_pcm))),
        "Stereo PCM must preserve the ordered channel samples and count frames per channel.");
  fixture->mounted = false;
  ui.Pump();
}

void TestInputSelection(testing::UiTest& ui) {
  const auto result = std::make_shared<std::optional<audio::detail::InputDevicesResult>>();
  fixture->tasks.Launch([result]() -> Task<void> { *result = co_await EnumerateInputDevicesAsync(); });
  PumpUntil(ui, [&] { return result->has_value(); });
  const auto inputs = Require(std::move(**result));
  Check(inputs == std::vector{test_microphone}, "Device discovery must return the backend's selectable inputs.");

  fixture->options = CaptureOptions{.active = true};
  fixture->mounted = true;
  PumpUntil(ui, [] { return fixture->session->Status().state == SessionState::Running; });
  Check(!fixture->run->Options().device_id, "An omitted device ID must reach the backend as the default selection.");
  auto reader = Require(fixture->session->OpenReader());
  const std::array<std::int16_t, 1> pcm{23};
  Check(fixture->run->Publish(mono_48k, CaptureClock::Monotonic, {2, 1}, pcm), "The default input rejected PCM.");

  fixture->options = CaptureOptions{.active = true, .device_id = inputs.front().id};
  PumpUntil(ui, [&] {
    return fixture->session->Status().state == SessionState::Running &&
           fixture->run->Options().device_id == inputs.front().id;
  });
  Check(Require(reader.Read()).has_value() && !Require(reader.Read()),
        "Changing the selected input must drain the old reader and start a new run.");

  fixture->options = CaptureOptions{.active = true, .device_id = "missing-microphone"};
  PumpUntil(ui, [] { return fixture->session->Status().state == SessionState::Failed; });
  Check(fixture->session->Status().error->code == CaptureErrorCode::Unavailable,
        "An unavailable microphone must fail capture without a default fallback.");
  const auto openings = fixture->openings.load();
  fixture->options = CaptureOptions{.active = true, .device_id = ""};
  ui.Pump();
  Check(fixture->session->Status().state == SessionState::Failed && fixture->openings == openings,
        "An empty microphone ID must fail before the backend starts.");
  fixture->mounted = false;
  ui.Pump();
}

void TestAsyncRead(testing::UiTest& ui) {
  const auto run = std::make_shared<CaptureRun>(nullptr);
  std::optional reader{Require(run->OpenReader())};
  bool canceled_read_resumed = false;
  const auto canceled = fixture->tasks.Launch([&]() -> Task<void> {
    static_cast<void>(co_await reader->ReadAsync());
    canceled_read_resumed = true;
  });
  ui.Pump();
  canceled.Cancel();

  std::optional<audio::detail::ReadResult> result;
  fixture->tasks.Launch([&]() -> Task<void> { result = co_await reader->ReadAsync(); });
  ui.Pump();
  const std::array<std::int16_t, 1> pcm{23};
  const MediaTimestamp pts{40, 1000};
  Check(!result && run->Publish(mono_48k, CaptureClock::Monotonic, pts, pcm), "The asynchronous read did not wait for PCM.");
  PumpUntil(ui, [&] { return result.has_value(); });
  const auto block = Require(std::move(*result));
  Check(!canceled_read_resumed && block && block->frame.pts == pts, "A canceled wait consumed the next PCM block.");

  auto stale = reader->ReadAsync();
  reader.reset();
  auto replacement = Require(run->OpenReader());
  result.reset();
  fixture->tasks.Launch([&result, task = std::move(stale)]() mutable -> Task<void> {
    result = co_await std::move(task);
  });
  PumpUntil(ui, [&] { return result.has_value(); });
  Check(!result->Succeeded() && result->Error().code == CaptureErrorCode::NotReady,
        "A deferred read must not attach to a replacement reader.");
}

struct TestDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("huxerui-audiocapture-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));

  TestDirectory() { Check(std::filesystem::create_directory(path), "Cannot create a test directory."); }
  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

AudioCaptureSession StartTestCapture(testing::UiTest& ui, CaptureOptions options = {}) {
  options.active = true;
  fixture->options = std::move(options);
  fixture->mounted = true;
  PumpUntil(ui, [] { return fixture->session->Status().state == SessionState::Running; });
  return *fixture->session;
}

void TestInputLevel(testing::UiTest& ui) {
  auto options = CaptureOptions{.active = true, .channel_layout = AudioChannelLayout::Stereo};
  const auto session = StartTestCapture(ui, options);
  const auto run = fixture->run;
  const auto format = *session.Status().format;
  const std::array<std::int16_t, 2> first{std::numeric_limits<std::int16_t>::min(), 16384};
  const std::array<std::int16_t, 6> rest{0, 16384, 0, 16384, 0, 16384};

  Check(run->Publish(format, CaptureClock::Monotonic, {2, 1}, first), "The unmonitored capture rejected PCM.");
  ui.Pump(100ms);
  Check(!session.InputLevel(), "Input level monitoring must be opt-in.");

  options.monitor_input_level = true;
  fixture->options = options;
  ui.Pump();
  Check(fixture->run == run && !session.InputLevel(), "Enabling monitoring must start an empty window on the same run.");
  Check(run->Publish(format, CaptureClock::Monotonic, {3, 1}, first), "The first measurement block was rejected.");
  Check(run->Publish(format, CaptureClock::Monotonic, {4, 1}, rest), "The second measurement block was rejected.");
  Check(!session.InputLevel(), "Input level must be published on the UI update interval.");
  ui.PumpUntil([&] { return session.InputLevel().has_value(); });
  const auto level = *session.InputLevel();
  Check(level.channel_count == 2 &&
        level.channels[0] == AudioInputLevel::Channel{.peak = 1.0F, .rms = 0.5F} &&
        level.channels[1] == AudioInputLevel::Channel{.peak = 0.5F, .rms = 0.5F},
        "Stereo levels must count samples across PCM blocks and handle negative full scale.");

  const std::array<std::int16_t, 2> silence{};
  Check(run->Publish(format, CaptureClock::Monotonic, {5, 1}, silence), "The silent measurement block was rejected.");
  ui.PumpUntil([&] { return session.InputLevel() == AudioInputLevel{.channel_count = 2}; });

  options.device_id = test_microphone.id;
  fixture->options = options;
  PumpUntil(ui, [&] { return fixture->run != run && session.Status().state == SessionState::Running; });
  Check(!session.InputLevel(), "Switching inputs must clear the previous measurement.");
  ui.PumpUntil([&] { return session.InputLevel().has_value(); });
  fixture->options = CaptureOptions{};
  PumpUntil(ui, [&] { return session.Status().state == SessionState::Stopped; });
  Check(!session.InputLevel(), "Stopping capture must clear the last measurement.");
}

void TestRecording(testing::UiTest& ui, const File& file, CodecId codec, ContainerFormat container) {
  auto pcm_format = mono_48k;
  pcm_format.sample_rate = codec == CodecId::PcmS16Le ? 16000 : 44100;
  pcm_format.channel_layout = AudioChannelLayout::Stereo;
  const auto rate = pcm_format.sample_rate;
  const auto channels = pcm_format.ChannelCount();
  const auto session = StartTestCapture(ui, {.sample_rate = rate, .channel_layout = pcm_format.channel_layout});
  const auto run = fixture->run;
  auto task = [&] {
    if (container == ContainerFormat::Adts)
      return session.RecordAsync(Require(file.OpenWrite()), codec, container);
    if (container == ContainerFormat::Mov) {
      auto stream = Require(file.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew));
      return session.RecordAsync(std::move(stream), codec, container);
    }
    return session.RecordAsync(file, codec, container);
  }();
  auto finished = std::make_shared<std::optional<CaptureResult<RecordingSummary>>>();
  fixture->tasks.Launch([task = std::move(task), finished]() mutable -> Task<void> {
    *finished = co_await std::move(task);
  });
  ui.Pump();

  constexpr std::int64_t frames = 3079;
  constexpr std::int64_t first_frames = 1300;
  constexpr std::int64_t paused_frames = 512;
  std::vector<std::int16_t> samples(frames * channels);
  for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<std::int16_t>(i * 3);
  const auto pcm = std::span(samples);
  const auto paused_pcm = pcm.first(paused_frames * channels);
  const std::int64_t origin = 128 * rate;
  Check(run->Publish(pcm_format, CaptureClock::Monotonic, {origin, rate}, pcm.first(first_frames * channels)),
        "First recording block was not accepted.");
  session.PauseRecording();
  Check(session.Status().recording_paused, "The session must report the recording pause.");
  Check(run->Publish(pcm_format, CaptureClock::Monotonic, {origin + first_frames, rate}, paused_pcm),
        "Capture must stay active during a recording pause.");

  session.ResumeRecording();
  Check(!session.Status().recording_paused, "The session must report the resumed recording.");
  Check(run->Publish(pcm_format, CaptureClock::Monotonic, {origin + first_frames + paused_frames, rate},
                     pcm.subspan(first_frames * channels)),
        "Resumed recording block was not accepted.");
  session.PauseRecording();
  Check(run->Publish(pcm_format, CaptureClock::Monotonic, {origin + frames + paused_frames, rate}, paused_pcm),
        "Capture must stay active until the paused recording stops.");
  fixture->options = CaptureOptions{};
  PumpUntil(ui, [&] { return finished->has_value() && session.Status().state == SessionState::Stopped; });
  Check(!session.Status().recording_paused, "Stopping capture must clear the recording pause.");
  const auto summary = Require(std::move(**finished));
  Check(summary.format == pcm_format && summary.duration == MediaTimestamp{frames, rate},
        "The summary must count accepted samples, excluding paused samples and encoder padding.");

  auto demuxer = Require(MediaDemuxer::Open(Require(file.OpenRandomAccess()), container));
  auto tracks = demuxer.Tracks();
  Check(demuxer.Format() == container && tracks.size() == 1 && tracks[0].descriptions.front().codec == codec &&
        tracks[0].descriptions.front().sample_rate == rate && tracks[0].descriptions.front().channel_count == channels,
        "Exported audio track metadata is wrong.");
#ifdef __APPLE__
  if (codec == CodecId::Aac && (container == ContainerFormat::Mp4 || container == ContainerFormat::Mov)) {
    Check(demuxer.PresentationRange(tracks.front().id) == MediaPresentationRange{{0, rate}, {frames, rate}},
          "The recording must preserve known AAC priming and padding.");
  }
#endif
  Bytes data;
  std::int64_t next = 0;
  while (auto item = Require(demuxer.Read())) {
    const auto* packet = std::get_if<ContainerPacket>(&*item);
    if (!packet) continue;
    Check(packet->pts == MediaTimestamp{next, rate}, "Export PTS must use the continuous sample grid from zero.");
    const auto bytes = packet->data.AsBytes();
    data.insert(data.end(), bytes.begin(), bytes.end());
    next += packet->duration->Rescale(rate)->ticks;
  }
  if (codec == CodecId::PcmS16Le) {
    Check(next == frames && std::ranges::equal(data, std::as_bytes(pcm)),
          "The recording round trip lost or changed PCM.");
  } else {
    Check(!data.empty() && next >= frames && next % 1024 == 0,
          "The container must contain the drained AAC access units.");
  }
}

void CheckRejectedRecording(testing::UiTest& ui, Task<CaptureResult<RecordingSummary>> task, CaptureErrorCode expected) {
  auto result = std::make_shared<std::optional<CaptureResult<RecordingSummary>>>();
  fixture->tasks.Launch([task = std::move(task), result]() mutable -> Task<void> {
    *result = co_await std::move(task);
  });
  PumpUntil(ui, [&] { return result->has_value(); });
  Check(!(*result)->Succeeded() && (*result)->Error().code == expected,
        "The recording task must report the expected error before waiting for PCM.");
}

void TestRecordingReaderOwnership(testing::UiTest& ui, const File& file) {
  const auto session = StartTestCapture(ui);
  const auto run = fixture->run;
  auto task = session.RecordAsync(file, CodecId::PcmS16Le, ContainerFormat::Wave);
  {
    auto reader = Require(session.OpenReader());
    session.PauseRecording();
    Check(!session.Status().recording_paused, "Recording commands must leave a raw PCM reader active.");
    CheckRejectedRecording(ui, std::move(task), CaptureErrorCode::ReaderInUse);
  }

  bool delivered = false;
  const auto canceled = fixture->tasks.Launch([session, file, &delivered]() -> Task<void> {
    static_cast<void>(co_await session.RecordAsync(file, CodecId::PcmS16Le, ContainerFormat::Wave));
    delivered = true;
  });
  ui.Pump();
  session.PauseRecording();
  Check(session.Status().recording_paused, "The recording must accept pause before receiving its first block.");

  fixture->options = CaptureOptions{.active = true, .monitor_input_level = true};
  ui.Pump();
  const std::array<std::int16_t, 1> paused_pcm{std::numeric_limits<std::int16_t>::min()};
  Check(run->Publish(mono_48k, CaptureClock::Monotonic, {2, 1}, paused_pcm), "The paused input PCM was rejected.");
  ui.PumpUntil([&] { return session.InputLevel().has_value(); });
  const auto level = *session.InputLevel();
  Check(fixture->run == run && session.Status().recording_paused && level.channel_count == 1 &&
        level.channels[0] == AudioInputLevel::Channel{.peak = 1.0F, .rms = 1.0F},
        "Enabling monitoring must measure the current input during a recording pause.");
  fixture->options = CaptureOptions{.active = true};
  ui.Pump();
  Check(fixture->run == run && session.Status().recording_paused && !session.InputLevel(),
        "Disabling monitoring must clear the level and preserve the paused recording.");

  canceled.Cancel();
  PumpUntil(ui, [&] { return !session.Status().recording_paused; });
  Check(!delivered && !file.Exists(), "Canceling a paused recording without retained PCM must discard its result.");

  auto reader = Require(session.OpenReader());
  const std::array<std::int16_t, 1> pcm{23};
  Check(fixture->run->Publish(mono_48k, CaptureClock::Monotonic, {3, 1}, pcm), "The new reader's PCM was rejected.");
  Check(Require(reader.Read()).has_value(), "A new reader must receive PCM after the paused recording is canceled.");
  fixture->options = CaptureOptions{};
  PumpUntil(ui, [&] { return session.Status().state == SessionState::Stopped; });
}

void TestUnsupportedRecording(testing::UiTest& ui, const File& path) {
  const auto session = StartTestCapture(ui);
  const auto file = path.Child("unsupported");
  CheckRejectedRecording(ui, session.RecordAsync(file, CodecId::Aac, ContainerFormat::Wave),
                         CaptureErrorCode::OutputFailed);
  CheckRejectedRecording(ui, session.RecordAsync(file, CodecId::Opus, ContainerFormat::Ogg),
                         CaptureErrorCode::Unavailable);
  Check(!file.Exists(), "An unsupported recording configuration must leave the file uncreated.");

  const auto stream_file = path.Child("sequential.mp4");
  CheckRejectedRecording(ui, session.RecordAsync(Require(stream_file.OpenWrite()), CodecId::Aac, ContainerFormat::Mp4),
                         CaptureErrorCode::OutputFailed);
  auto empty_stream = Require(stream_file.OpenRandomAccess());
  Check(Require(empty_stream.Size()) == 0, "A rejected sequential recording must leave the stream empty.");
}

} // namespace

int main() {
  try {
    TestTimestampsAndQueue();
    Fixture current;
    fixture = &current;
    const Application app{TestApp, {.application_hooks = {[](ApplicationContext& context) {
      context.RegisterPlatformModule<audio::detail::CaptureBackend>(audio::detail::capture_module,
          [](Runtime&) -> audio::detail::CaptureBackend {
        return {.start = [](const std::shared_ptr<CaptureRun>& run, audio::detail::CaptureCompleted completed)
                    -> std::function<void()> {
          ++fixture->openings;
          fixture->run = run;
          if (const auto& id = run->Options().device_id; id && *id != test_microphone.id) {
            completed(CaptureError{CaptureErrorCode::Unavailable, "The selected microphone is unavailable."});
            return {};
          }
          const auto format = run->RequestedFormat(mono_48k);
          const std::array<std::int16_t, 2> pcm{23, 51};
          run->Publish(format, CaptureClock::Monotonic, {1, 1}, std::span(pcm).first(format.ChannelCount()));
          return [completed = std::move(completed)] { completed(std::nullopt); };
        }, .use_sdk_permission = fixture->use_sdk_permission,
          .enumerate = []() -> Task<audio::detail::InputDevicesResult> {
            co_return audio::detail::InputDevicesResult::Success({test_microphone});
          }};
      });
    }}}};
    testing::UiTest ui(app);
    ui.UpdateWindowLifecycleState(WindowLifecycleState::Active);
    ui.Pump();
    TestAuthorizationAndLifetime(ui);
    TestCaptureFormat(ui);
    TestInputSelection(ui);
    TestAsyncRead(ui);
    TestInputLevel(ui);
    const TestDirectory directory;
    const File path(directory.path.u8string());
    TestRecording(ui, path.Child("test.wav"), CodecId::PcmS16Le, ContainerFormat::Wave);
    TestRecording(ui, path.Child("test-rf64.wav"), CodecId::PcmS16Le, ContainerFormat::Rf64);
    TestRecording(ui, path.Child("test-pcm.mov"), CodecId::PcmS16Le, ContainerFormat::Mov);
    TestRecording(ui, path.Child("test.aac"), CodecId::Aac, ContainerFormat::Adts);
    TestRecording(ui, path.Child("test.m4a"), CodecId::Aac, ContainerFormat::Mp4);
    TestRecording(ui, path.Child("test-aac.mov"), CodecId::Aac, ContainerFormat::Mov);
    TestRecordingReaderOwnership(ui, path.Child("reader-ownership.wav"));
    TestUnsupportedRecording(ui, path);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
