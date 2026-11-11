# Lib-AudioCapture

Lib-AudioCapture is a C++ microphone capture library for HuxerUI. It provides composition-owned capture
sessions, timestamped PCM for audio/video synchronization, and WAVE and AAC/ADTS recording export.

Link `HuxerUI::AudioCapture` and use the `huxerui::audio` namespace. The library uses Lib-Media's
`AudioFrame`, `AudioFormat`, and `MediaTimestamp` for PCM and delegates encoding and container writing
to Lib-Media. [Public API](include/huxerui/audiocapture.h)

## Integration

Use the HuxerUI SDK and C++20. Add one of the following pairs of `huxerui_use_library()` calls after
creating your application target. Register `HuxerUI::Media` first so the SDK includes both libraries
in the iOS application core. Choose compatible revisions of Lib-Media and Lib-AudioCapture.

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
from a user action and activate capture after a grant. The library checks authorization on every start and
foreground restart.

- **Android** requires API 26 or later. The library manifest declares `RECORD_AUDIO`.
- **iOS and macOS** require `NSMicrophoneUsageDescription` in the app's Info.plist. Enable Audio Input in the
  applicable macOS signing capabilities. On iOS, AVFoundation configures the app audio session for recording
  using its default capture session behavior.
- **Windows** requires desktop microphone access in Windows privacy settings. A packaged app must declare the
  microphone capability where its packaging model requires it.
- **Linux** requires ALSA development headers and an ALSA default input with monotonic PCM timestamps. ALSA
  enforces device access when opening the input.
- **Web** requires a secure context and a browser that exposes timestamped audio through `MediaStreamTrackProcessor`
  and `AudioData` on the page. Enable `active` from the start action so the browser can handle authorization through
  `getUserMedia()`. The backend reports `Unavailable` when the capture APIs are missing.

## Capture and export

Call `huxerui::audio::UseAudioCapture({.active = active})` in the component that owns the microphone.
`Status()` observes session state and the negotiated PCM format. When the session is Starting or Running,
call `OpenReader()` on the UI thread to attach the run's sole reader. Await `ReadAsync()` to receive retained
PCM without blocking. On desktop and mobile, the reader also supports blocking `Read()` on a worker.
Capture produces interleaved S16, mono at 48 kHz on Android, Linux, and Windows, and mono or stereo at the
input sample rate on iOS, macOS, and Web. The format and clock domain stay fixed for one run.

Transfer the reader to a recording task launched through `TaskScope` to export a standalone recording.

| Function | Destination | Output |
| --- | --- | --- |
| `RecordWaveAsync` | New `File`, or empty writable `RandomAccessStream` | PCM WAVE, backpatched on finish |
| `RecordAacAsync` | Fresh `OutputStream` | AAC-LC in sequential ADTS, no seeking required |

Export uses a worker on desktop and mobile and the browser event loop on Web. It uses a zero-based,
continuous sample timeline. On Web, record WAVE to a temporary file and use `FilePicker` to save it.
AAC accepts 44.1 or 48 kHz PCM when the Lib-Media encoder supports the configuration. ADTS includes
encoder delay and padding in its access units. The returned duration counts the original PCM samples.

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

The preview and standalone library builds use a sibling `Lib-Media` checkout by default. Set
`HUXERUI_AUDIOCAPTURE_MEDIA_DIR` as a CMake cache variable or environment variable to select another checkout.

Run the recording preview with the HuxerUI CLI. Use the platform name for your target in place of `macos`.
[Preview source](examples/preview)

```sh
cd examples/preview
huxerui run macos --profile debug
```

**Start recording** requests access and starts a local WAVE recording. **Stop recording** finalizes the file,
and **Export WAVE** opens the save picker where supported.

Configure with `-DHUXERUI_AUDIOCAPTURE_BUILD_TESTS=ON` and run `ctest --test-dir <build-dir> --output-on-failure`.
The contract test uses synthetic PCM to check timestamps, queue ownership and interruption, authorization
and unmount, and WAVE and AAC/ADTS export through Lib-Media. Run on a host with the HuxerUI testing component
and an available Lib-Media AAC encoder.

## License

Lib-AudioCapture is licensed under the [MIT License](LICENSE).
