# Lib-AudioCapture

Lib-AudioCapture is a C++ microphone capture library for HuxerUI. It provides composition-owned capture
sessions, timestamped PCM for audio/video synchronization, and recording export with selectable codecs
and containers.

Link `HuxerUI::AudioCapture` and use the `huxerui::audio` namespace. The library uses Lib-Media's
`AudioFrame`, `AudioFormat`, and `MediaTimestamp` for PCM and delegates encoding and container writing
to Lib-Media. [Public API](include/huxerui/audiocapture.h)

## Integration

Use C++20 and a HuxerUI SDK that supports `huxerui_use_library(PUBLIC)`. AudioCapture declares Media as a public dependency, so registering AudioCapture also brings Media into the build. For adjacent Lib-AudioCapture and Lib-Media checkouts, register AudioCapture after creating your application target.

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::AudioCapture
    PATH "${AUDIOCAPTURE_SOURCE_DIR}"
)
```

Set `HUXERUI_AUDIOCAPTURE_MEDIA_DIR` to select another Media checkout. When pinning both libraries by URL, register the Media revision first so AudioCapture can reuse its target.

### Pin by commit

Set `LIB_MEDIA_COMMIT` and `LIB_AUDIOCAPTURE_COMMIT` to full 40-character Git commit SHAs.

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::Media
    URL "https://github.com/HuxerUI/Lib-Media.git"
    COMMIT "${LIB_MEDIA_COMMIT}"
)
huxerui_use_library(your_app
    TARGET HuxerUI::AudioCapture
    URL "https://github.com/HuxerUI/Lib-AudioCapture.git"
    COMMIT "${LIB_AUDIOCAPTURE_COMMIT}"
)
```

### Pin by tag

Set `LIB_MEDIA_TAG` and `LIB_AUDIOCAPTURE_TAG` to plain Git tag names from their respective repositories.

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::Media
    URL "https://github.com/HuxerUI/Lib-Media.git"
    TAG "${LIB_MEDIA_TAG}"
)
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

## Capture and export

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

When the session is Starting or Running, call `OpenReader()` on the UI thread to attach the run's sole
reader. Await `ReadAsync()` to receive retained PCM without blocking. On desktop and mobile, the reader
also supports blocking `Read()` on a worker.
Capture produces interleaved S16. By default Android, Linux, and Windows use 48 kHz mono, and iOS, macOS,
and Web use the input's sample rate with mono or stereo PCM. The format and clock domain stay fixed for one run.

Set `CaptureOptions::monitor_input_level` to `true` and read `session.InputLevel()` during composition to
observe per-channel peak and RMS levels. The values range from `0` for silence to `1` for full scale and
update about every 50 ms when PCM is available. Monitoring measures the live input before the reader queue,
including while recording is paused. Changing the monitoring option takes effect on the current run.
The level is empty until measured and is cleared when monitoring or the capture run ends.

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
The returned duration counts the original PCM samples. Unsupported codecs or combinations fail before
reading PCM or writing output.

Call `session.PauseRecording()` and `session.ResumeRecording()` from UI actions. Observe the pause through
`session.Status().recording_paused`. Pausing keeps the microphone open and skips new PCM blocks before
they enter the queue. Queued blocks still reach the file. Resuming continues the same recording, with its
timeline and duration based on retained samples. You can also stop while paused.

Setting `active = false` seals the input queue. The reader drains accepted blocks and reaches EOF, then the
recording task finishes and closes the destination before reporting success. Backgrounding or unmounting
interrupts the run. A foreground restart requires a new reader. Device failure, a detected discontinuity, or
queue overflow fails the reader and any active export. Queuing starts when a reader attaches, with a limit of
128 blocks and 512 KiB of PCM. A failed or canceled export can leave partial output. Canceling an export
releases its reader once outstanding I/O returns.

## Synchronize with Camera

`CapturedAudio::frame.pts` is the capture time of the first sample frame in the block. The clock domain is
explicit in `CapturedAudio::clock`; the `MediaTimestamp::timescale` only describes the units.

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

## Preview and tests

AudioCapture uses a sibling `Lib-Media` checkout by default. Set `HUXERUI_AUDIOCAPTURE_MEDIA_DIR` as a CMake cache variable or environment variable to select another checkout.

Run the recording preview with the HuxerUI CLI. Use the platform name for your target in place of `macos`.
[Preview source](examples/preview)

```sh
cd examples/preview
huxerui run macos --profile debug
```

**Start recording** requests access and starts a local WAVE recording. The input meter shows the RMS of the
active microphone. **Pause recording** and **Resume recording** control which audio reaches the file.
**Stop recording** finalizes the file, and **Export WAVE** opens the save picker where supported.

Configure with `-DHUXERUI_AUDIOCAPTURE_BUILD_TESTS=ON` and run `ctest --test-dir <build-dir> --output-on-failure`.
The contract test uses synthetic PCM to check timestamps, queue ownership and interruption, authorization
and unmount, PCM export to WAVE, RF64, and MOV, and AAC export to ADTS, M4A, and MOV. Run on a host with the
HuxerUI testing component and an available Lib-Media AAC encoder.

## License

Lib-AudioCapture is licensed under the [MIT License](LICENSE).
