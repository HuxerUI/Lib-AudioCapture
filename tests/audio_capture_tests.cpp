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

#include "capture_run.h"

using namespace huxerui;
using namespace huxerui::audio;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace {

using audio::detail::CaptureRun;
using audio::detail::mono_48k;

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
}

struct Fixture {
  State<bool> mounted;
  State<bool> active;
  TaskScope tasks;
  std::optional<AudioCaptureSession> session;
  std::atomic<unsigned> openings = 0;
};

Fixture* fixture;

[[huxerui::composable]]
View CaptureOwner() {
  fixture->session = UseAudioCapture({.active = fixture->active});
  return Text("Capture owner");
}

View TestApp() {
  fixture->mounted = UseState(true);
  fixture->active = UseState(false);
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
  fixture->active = true;
  PumpUntil(ui, [] { return fixture->session->Status().state == SessionState::Failed; });
  Check(fixture->session->Status().error->code == CaptureErrorCode::Unavailable && fixture->openings == 0,
        "The microphone must not open on a host without permission support.");
  fixture->mounted = false;
  ui.Pump();
  Check(fixture->session->Status().state == SessionState::Closed && !fixture->session->OpenReader().Succeeded(),
        "Retaining a handle must not extend its composition lifetime.");
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

void TestRecording(testing::UiTest& ui, const File& directory, ContainerFormat format) {
  const auto run = std::make_shared<CaptureRun>(nullptr);
  auto reader = Require(run->OpenReader());
  const auto file = directory.Child(format == ContainerFormat::Wave ? "test.wav" : "test.aac");
  auto task = format == ContainerFormat::Wave ? RecordWaveAsync(std::move(reader), file)
                                              : RecordAacAsync(std::move(reader), Require(file.OpenWrite()));
  auto finished = std::make_shared<std::optional<CaptureResult<RecordingSummary>>>();
  fixture->tasks.Launch([task = std::move(task), finished]() mutable -> Task<void> {
    *finished = co_await std::move(task);
  });

  std::array<std::int16_t, 3079> samples{};
  for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<std::int16_t>(i * 3);
  const auto pcm = std::span(samples);
  Check(run->Publish(mono_48k, CaptureClock::BootTime, {128000000, 1000000}, pcm.first(1300)),
        "First recording block was not accepted.");
  Check(run->Publish(mono_48k, CaptureClock::BootTime, {128027085, 1000000}, pcm.subspan(1300)),
        "Second recording block was not accepted.");
  run->End();
  PumpUntil(ui, [&] { return finished->has_value(); });
  const auto summary = Require(std::move(**finished));
  Check(summary.format == mono_48k && summary.duration == MediaTimestamp{samples.size(), 48000},
        "The summary must count all accepted samples, excluding encoder padding.");

  auto demuxer = Require(MediaDemuxer::Open(Require(file.OpenRandomAccess()), format));
  auto tracks = demuxer.Tracks();
  Check(tracks.size() == 1 && tracks[0].descriptions.front().sample_rate == 48000 &&
        tracks[0].descriptions.front().channel_count == 1, "Exported audio track metadata is wrong.");
  Bytes data;
  std::int64_t next = 0;
  while (auto item = Require(demuxer.Read())) {
    const auto* packet = std::get_if<ContainerPacket>(&*item);
    if (!packet) continue;
    Check(packet->pts == MediaTimestamp{next, 48000}, "Export PTS must use the continuous sample grid from zero.");
    const auto bytes = packet->data.AsBytes();
    data.insert(data.end(), bytes.begin(), bytes.end());
    next += packet->duration->Rescale(48000)->ticks;
  }
  if (format == ContainerFormat::Wave) {
    Check(next == samples.size() && std::ranges::equal(data, std::as_bytes(pcm)),
          "WAVE round trip lost or changed PCM.");
  } else {
    Check(!data.empty() && next >= samples.size() && next % 1024 == 0,
          "ADTS must contain the drained AAC access units.");
  }
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
        return {.start = [](const std::shared_ptr<CaptureRun>&, audio::detail::CaptureCompleted completed)
                    -> std::function<void()> {
          ++fixture->openings;
          completed(CaptureError{CaptureErrorCode::CaptureFailed, "The test host opened a microphone."});
          return {};
        }};
      });
    }}}};
    testing::UiTest ui(app);
    ui.UpdateWindowLifecycleState(WindowLifecycleState::Active);
    ui.Pump();
    TestAuthorizationAndLifetime(ui);
    TestAsyncRead(ui);
    const TestDirectory directory;
    const File path(directory.path.u8string());
    TestRecording(ui, path, ContainerFormat::Wave);
    TestRecording(ui, path, ContainerFormat::Adts);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
