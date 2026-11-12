# Lib-AudioCapture

Lib-AudioCapture is a C++ microphone capture library for HuxerUI. It provides composition-owned capture
sessions, timestamped PCM for audio/video synchronization, and recording export with selectable codecs
and containers.

Include `<huxerui/audiocapture.h>` and use the `huxerui::audio` namespace. The library uses Lib-Media's
`AudioFrame`, `AudioFormat`, and `MediaTimestamp` for PCM and delegates encoding and container writing
to Lib-Media. [Public API](include/huxerui/audiocapture.h)

## Integration

Use C++20 and a HuxerUI SDK that supports the `PUBLIC` option of `huxerui_use_library`. After creating
your application target, add `HuxerUI::AudioCapture` using one of the forms below. AudioCapture links
`HuxerUI::Media` publicly, so the application gets Media through AudioCapture.

AudioCapture reuses an existing `HuxerUI::Media` target or a `Lib-Media` checkout alongside its own
source. If neither is available, CMake fetches the Lib-Media commit pinned by AudioCapture.

### Local checkout

Set `AUDIOCAPTURE_SOURCE_DIR` to the local Lib-AudioCapture checkout.

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::AudioCapture
    PATH "${AUDIOCAPTURE_SOURCE_DIR}"
)
```

### Pin by commit

Set `LIB_AUDIOCAPTURE_COMMIT` to a full 40-character Git commit SHA.

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::AudioCapture
    URL "https://github.com/HuxerUI/Lib-AudioCapture.git"
    COMMIT "${LIB_AUDIOCAPTURE_COMMIT}"
)
```

### Pin by tag

Set `LIB_AUDIOCAPTURE_TAG` to a plain Git tag name from the Lib-AudioCapture repository.

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::AudioCapture
    URL "https://github.com/HuxerUI/Lib-AudioCapture.git"
    TAG "${LIB_AUDIOCAPTURE_TAG}"
)
```

### Register the application hook

Add `huxerui::audio::Install` to `AppOptions::application_hooks`. `App` is the application's root view function.

```cpp
#include <huxerui/audiocapture.h>
#include <huxerui/huxerui.h>

huxerui::View App();

const huxerui::Application application{
    App,
    {.application_hooks = {huxerui::audio::Install}},
};
```

## Platform requirements

On Android, iOS, macOS, and Windows, request `Permission::Microphone` through `ApplicationHandle::RequestPermissionAsync`
from a user action before listing devices or activating capture. The library checks authorization on each query,
start, and foreground restart.

- **Android** requires API 26 or later. The library manifest declares `RECORD_AUDIO`.
- **iOS and macOS** require `NSMicrophoneUsageDescription` in the app's Info.plist. Enable Audio Input in the
  applicable macOS signing capabilities. iOS queries input ports during an active capture session. If capture
  is idle, discovery briefly opens the microphone and releases it before returning. An explicit input uses the
  app's shared audio route, so overlapping AudioCapture runs with a conflicting selection fail.
- **Windows** requires desktop microphone access in Windows privacy settings. A packaged app must declare the
  microphone capability where its packaging model requires it.
- **Linux** requires ALSA development headers and an ALSA input with monotonic PCM timestamps. Discovery uses
  configured ALSA capture endpoints. ALSA enforces device access when opening the input.
- **Web** requires a secure context and a browser that exposes timestamped audio through `MediaStreamTrackProcessor`
  and `AudioData` on the page. Enable `active` from the start action so the browser can handle authorization through
  `getUserMedia()`. The backend reports `Unavailable` when the capture APIs are missing.

## Configure capture

Call `huxerui::audio::EnumerateInputDevicesAsync()` from a user action through `TaskScope` to obtain a snapshot
of the selectable inputs. Display each `InputDevice::name` and pass its `id` to `CaptureOptions::device_id` when
selected. On Web the query may prompt and open a temporary microphone track to expose the device IDs. It stops
that track before returning the list.

Omit `device_id` to use the platform default. Explicit selection reports `CaptureErrorCode::Unavailable` if
the requested input cannot be selected. Query again after reconnecting a device or when a saved ID stops working.

Call `huxerui::audio::UseAudioCapture()` in the component that owns the microphone. Set
`CaptureOptions::sample_rate` in Hz and `CaptureOptions::channel_layout` to Lib-Media's
`AudioChannelLayout::Mono` or `AudioChannelLayout::Stereo`. Each omitted field uses its platform default.

```cpp
auto session = huxerui::audio::UseAudioCapture({
    .active = active,
    .sample_rate = 44'100,
    .channel_layout = huxerui::media::AudioChannelLayout::Stereo,
    .device_id = selected_device_id,
});
```

An explicit rate or layout must be supported by the capture backend or its PCM converter for the run to
start. A backend may derive both stereo channels from a mono input. Inspect `Status().format` for the
delivered format and `Status().error` if startup fails. Changing the input, rate, or layout during capture
starts a new run after the current backend stops. The old reader drains its queued PCM, and the new run
accepts a new reader.

Set `CaptureOptions::monitor_input_level` to `true` and read `session.InputLevel()` during composition to
observe per-channel peak and RMS levels. Levels range from `0` for silence to `1` for full scale and
update about every 50 ms when PCM is available. Monitoring measures the microphone signal before any
reader or processor, including while recording is paused. It can be enabled or disabled on the current run.
`InputLevel()` is empty until measured and clears when monitoring or capture ends.

Set `active = false` to release the microphone and let the reader drain. Backgrounding or unmounting
interrupts capture. A foreground restart opens a new run and requires a new reader.

## Read PCM

When the session is Starting or Running, call `OpenReader()` on the UI thread to attach the run's sole
reader. Await `ReadAsync()` to receive retained PCM without blocking. On desktop and mobile, the reader
also supports blocking `Read()` on a worker.

Capture produces interleaved S16. By default Android, Linux, and Windows use 48 kHz mono, and iOS, macOS,
and Web use the input's sample rate with mono or stereo PCM. The format and clock domain stay fixed for one run.

Call `reader.FinishInput()` to stop accepting new PCM while keeping the microphone active. Continue
reading until EOF to drain queued PCM and processor output. Release the reader before opening another one.

For a timed stop, pass a `MediaTimestamp` on the microphone's capture clock to `FinishInput()`. The reader
retains complete sample frames ending at or before that cutoff, including in queued blocks, and stops
accepting PCM when capture reaches the cutoff or ends cleanly. The request fails if PCM already dequeued
extends past the cutoff. Call `FinishInput()` without a time to seal input immediately while waiting for a cutoff.

The reader queue holds up to 128 blocks and 512 KiB of PCM. Device failure, discontinuity, or queue
overflow fails a reader whose input is still open.

## Record and export

Launch `session.RecordAsync()` through `TaskScope` to export a standalone recording. The task attaches
the run's sole reader on start and reports `ReaderInUse` if the reader is already occupied. Select the
codec and container with Lib-Media's `CodecId` and `ContainerFormat`.

| `CodecId` | `ContainerFormat` | Destination |
| --- | --- | --- |
| `PcmS16Le` | `Wave`, `Rf64`, `Mov` | New `File` or empty writable `RandomAccessStream` |
| `Aac` | `Mp4` (audio-only M4A), `Mov` | New `File` or empty writable `RandomAccessStream` |
| `Aac` | `Adts` | New `File`, empty writable `RandomAccessStream`, or fresh sequential `OutputStream` |

For a `.m4a` file, pass `Aac` with `Mp4`.

```cpp
auto result = co_await session.RecordAsync(
    destination, huxerui::media::CodecId::Aac, huxerui::media::ContainerFormat::Mp4);
```

Export uses a worker on desktop and mobile and the browser event loop on Web. It uses a zero-based,
continuous PCM sample timeline. On Web, record to a temporary file and use `FilePicker` to save it.
AAC-LC accepts 44.1 or 48 kHz mono or stereo PCM when the Lib-Media encoder supports the configuration.
An optional bitrate in bits per second applies to AAC and defaults to 128000. MP4 and MOV retain known
AAC encoder delay and padding as presentation trim. ADTS carries all encoded access units in order.
The returned duration counts the PCM samples submitted to the writer. Unsupported codecs or combinations fail before
reading PCM or writing output.

Call `session.PauseRecording()` and `session.ResumeRecording()` from UI actions. Observe the pause through
`session.Status().recording_paused`. Pausing keeps the microphone open and skips new PCM blocks before
they enter the queue. Queued blocks still reach the file. Resuming continues the same recording, with its
timeline and duration based on retained samples.

Call `session.StopRecording()` from a UI action to finish a running or paused recording while keeping microphone
capture and any enabled input level monitoring active. The recording drains accepted PCM, processor output, and
the encoder before closing the destination. Await the original `RecordAsync()` task for the result before
starting another recording.

A failed or canceled export can leave partial output. Canceling an export releases its reader once
outstanding I/O returns.

## Process PCM

Set `CaptureOptions::pcm_processor` to a shared factory that creates a `PcmProcessor` for the reader's
`AudioFormat`. Reuse the same factory across compositions. Replacing it starts a new run and lets the
previous reader drain its processor. `Read`, `ReadAsync`, and `RecordAsync` all receive the processed PCM.

```cpp
static const auto processor = std::make_shared<const huxerui::audio::PcmProcessorFactory>(
    [](const huxerui::media::AudioFormat& format) { return std::make_unique<VoiceProcessor>(format); });
```

Implement `Process()` to accept each interleaved S16 input block and `TryRead()` to return the next
available output block. A processor can accumulate several inputs or return multiple blocks from one input.
The sample rate and channel layout stay fixed. At clean EOF, `Finish()` makes the remaining output available
to `TryRead()`. During recording, the processor receives only PCM admitted by the pause control. Input
level monitoring still measures the microphone signal.

| `PcmTiming` | Output timestamps |
| --- | --- |
| `CaptureTime` | Preserves the input clock and sample correspondence. The reader splits output at input timestamp boundaries, including gaps from paused recording. Output and input must contain the same number of frames at EOF. |
| `StreamTime` | Counts output sample frames from zero, allowing effects that change duration. `CapturedAudio::clock` is empty. |

## Synchronize with Camera

When `CapturedAudio::clock` has a value, `CapturedAudio::frame.pts` is the capture time of the first sample
frame in that clock domain. Use this mode to align the audio with camera timestamps. `MediaTimestamp::timescale`
describes the timestamp units.

| Platform | First-sample timestamp | Compatible video timestamp |
| --- | --- | --- |
| Android | `BootTime`. AAudio correlates a sample position to `CLOCK_BOOTTIME`. AudioCapture offsets that correlation by the sample distance to the first frame read. Blocks before the initial valid correlation are skipped. [AAudio timestamps](https://developer.android.com/ndk/reference/group/audio#aaudiostream_gettimestamp) | Camera2 sensor timestamps when the device reports `SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME`. An `UNKNOWN` source needs a separate clock correlation. [Camera2 timestamp source](https://developer.android.com/reference/android/hardware/camera2/CameraCharacteristics#SENSOR_INFO_TIMESTAMP_SOURCE) |
| iOS and macOS | `CoreMediaHostTime`. The audio sample buffer PTS is converted from the capture session synchronization clock to the CoreMedia host clock. [AVCaptureSession synchronization clock](https://developer.apple.com/documentation/avfoundation/avcapturesession/synchronizationclock) | Video sample buffer PTS mapped from its own session clock to the same host clock. |
| Windows | `QueryPerformanceCounter`. WASAPI supplies the first frame's QPC time in 100 ns units. [WASAPI capture packet timing](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer) | Video timing correlated to QPC by the video backend. A stream-relative Media Foundation PTS needs a separate clock correlation. |
| Linux | `Monotonic`. The first unread frame time is estimated from the ALSA status timestamp and capture delay, including input latency when reported by the driver. [ALSA PCM timestamping](https://www.kernel.org/doc/html/latest/sound/designs/timestamping.html) | Video timing mapped to `CLOCK_MONOTONIC` on the same host. |
| Web | `MediaStreamTrackTime`. The browser supplies the first sample time in `AudioData.timestamp`, in microseconds. [WebCodecs timestamp semantics](https://www.w3.org/TR/webcodecs/#presentation-timestamp) | Video timestamps whose origin has been verified to match or mapped to the audio timeline. The browser defines the origin. |

For a recorder combining Lib-Camera video with AudioCapture PCM, use Lib-Media to encode and multiplex
both tracks. Map both inputs to the same clock and subtract one recording origin to preserve their start
offset. For Lib-Media's AAC encoder, align the first PCM frame to the resulting sample grid, then advance
by the number of accepted samples. Keep the original capture PTS for drift measurement. The recorder
owns resampling and discontinuity policy. Clock precision and overrun detection depend on the platform
input and driver.

For a shared stop time, map it to the microphone's capture clock and pass it to `AudioReader::FinishInput()`.
When processing PCM for the recorder, use `PcmTiming::CaptureTime` to preserve capture timestamps.

## Preview and tests

Run the recording preview with the HuxerUI CLI. Use the platform name for your target in place of `macos`.
[Preview source](examples/preview)

```sh
cd examples/preview
huxerui run macos --profile debug
```

The preview opens **Voice Studio**. Pick Original, Deep, Chipmunk, Robot, Radio, or Slow Motion before
recording. The input graph shows microphone RMS and peak levels during capture. Pause and resume to
control which PCM reaches the take. Stop to drain the processor, then play, seek, or export the WAVE file
from the latest take.

Deep and Chipmunk use [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) to
shift pitch and formants while keeping the capture duration. Slow Motion preserves pitch and produces a
take about 25% longer. The preview fetches pinned versions of Stretch and Signalsmith Linear at configure
time. [Third-party notices](examples/preview/resources/raw/third_party_notices.txt)

Configure with `-DHUXERUI_AUDIOCAPTURE_BUILD_TESTS=ON` and run `ctest --test-dir <build-dir> --output-on-failure`.
The contract test uses synthetic PCM to check timestamps, reader cutoffs, processing and tail drain,
capture lifecycle, and PCM and AAC export. It requires the HuxerUI testing component and an available
Lib-Media AAC encoder.

## License

Lib-AudioCapture is licensed under the [MIT License](LICENSE).
