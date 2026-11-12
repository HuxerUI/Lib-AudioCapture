#pragma once

/// @file
/// @brief Composition-owned microphone capture and audio recording export.
///
/// Include <huxerui/audiocapture.h> and link HuxerUI::AudioCapture. Register Install() through
/// AppOptions::application_hooks and activate UseAudioCapture() from a user action. Obtain Permission::Microphone
/// first on Android, iOS, macOS, and Windows. On Linux the audio device enforces access. On Web the browser
/// requests permission when capture starts. Web requires a secure context and a track processor supporting audio.
/// Read PCM asynchronously, read on a worker on desktop and mobile, or export with a recording Task.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/media_container.h>
#include <huxerui/media_format.h>
#include <huxerui/stream.h>
#include <huxerui/task.h>

namespace huxerui::audio {

/// @brief Clock domain of a microphone capture timestamp.
///
/// media::MediaTimestamp::timescale defines the units. Compare capture times only after mapping the
/// sources to the same clock domain and origin.
enum class CaptureClock {
  /// AAudio sample position correlated to CLOCK_BOOTTIME, the clock used by SystemClock.elapsedRealtimeNanos().
  BootTime,
  /// Capture session sample time mapped to CMClockGetHostTimeClock().
  CoreMediaHostTime,
  /// WASAPI capture packet time on the QueryPerformanceCounter clock in 100 ns units.
  QueryPerformanceCounter,
  /// CLOCK_MONOTONIC time estimated from the PCM status timestamp and input delay reported by ALSA.
  Monotonic,
  /// Browser capture timestamp from the audio track processor, in microseconds.
  /// The browser defines the origin. Establish a shared origin before comparing different tracks.
  MediaStreamTrackTime,
};

/// @brief One retained PCM block and the clock domain of its first-sample timestamp.
///
/// Successful reads provide immutable, interleaved S16 PCM with one plane. Copying the block shares
/// its storage. The plane remains readable after the reader or session closes.
/// @see AudioReader::Read(), media::AudioFrame
struct CapturedAudio {
  /// PCM samples and the first sample frame's time on the reader's timeline.
  media::AudioFrame frame;
  /// Clock domain of a capture timestamp. An empty value denotes the reader's zero-based output timeline.
  std::optional<CaptureClock> clock;
};

/// @brief Portable categories for capture, reader, and recording failures.
enum class CaptureErrorCode {
  /// The platform, microphone, input format, permission capability, or recording codec is unavailable.
  Unavailable,
  /// The application must obtain a microphone permission decision.
  PermissionRequired,
  /// Microphone access was denied or restricted.
  PermissionDenied,
  /// The capture run cannot accept a reader, or the reader has been moved from.
  NotReady,
  /// The current run already has a reader, or the reader already has a pending asynchronous read.
  ReaderInUse,
  /// A run was interrupted, for example by backgrounding, unmount, or device disconnection.
  /// Read() also reports this category when its stop token is requested.
  Interrupted,
  /// Capture could not start or the backend or processor could not supply valid PCM and timing.
  CaptureFailed,
  /// The backend detected missing samples or a change in the format or sample timeline.
  Discontinuity,
  /// The reader or processor exceeded a PCM buffering limit.
  QueueOverflow,
  /// The capture ended before the recording received any PCM.
  NoAudio,
  /// The codec and container are incompatible for recording, or the destination could not be opened, written, or finalized.
  OutputFailed,
  /// The AAC encoder rejected its configuration or failed during encoding or drain.
  EncodeFailed,
  /// A reader control argument is invalid.
  InvalidArgument,
};

/// @brief An operation failure with a portable category and a human-readable diagnostic.
struct CaptureError {
  /// Stable category for application decisions.
  CaptureErrorCode code;
  /// Diagnostic information about the failure. Use code for programmatic handling.
  std::string message;

  /// Compares the category and diagnostic.
  bool operator==(const CaptureError&) const = default;
};

/// @brief An owned success value or CaptureError using the SDK Result contract.
///
/// Check Succeeded() before calling Value() or Error(). Accessing the other branch throws
/// std::logic_error. Move the result to transfer a move-only success value such as AudioReader.
/// @tparam T Type stored on success.
template<class T> using CaptureResult = Result<T, CaptureError>;

/// @brief Timeline for PCM returned by a processor.
enum class PcmTiming {
  /// Output frame i corresponds to input frame i after compensating for algorithmic delay.
  /// Output can only cover frames already submitted. At graceful EOF, the total frame counts must agree.
  CaptureTime,
  /// Output frames advance a zero-based timeline. The processor may change the total duration.
  StreamTime,
};

/// @brief Owned interleaved S16 PCM in the processor's configured format.
using PcmBlock = std::vector<std::int16_t>;

/// @brief Streaming PCM processing for one reader.
///
/// Calls are serialized on the reader's execution context. The factory, processing, and drain run on
/// a worker on native platforms and on the browser event loop on Web. The capture format stays fixed.
/// An error or exception ends the reader and releases its capture slot.
class PcmProcessor {
public:
  virtual ~PcmProcessor() = default;

  /// Declares the timestamp contract for this instance.
  virtual PcmTiming Timing() const noexcept = 0;

  /// Accepts all input samples. The span is valid for the duration of the call.
  virtual CaptureResult<void> Process(std::span<const std::int16_t> pcm) = 0;

  /// Returns one nonempty block of complete frames, up to 512 KiB, or an empty value when drained.
  /// Before Finish(), an empty value requests more input. After Finish(), it indicates end of output.
  virtual CaptureResult<std::optional<PcmBlock>> TryRead() = 0;

  /// Called once when the reader reaches a clean input EOF. Remaining output must be available to TryRead() on return.
  virtual CaptureResult<void> Finish() = 0;
};

/// @brief Creates a new processing instance for a reader when its first PCM format is known.
using PcmProcessorFactory = std::function<std::unique_ptr<PcmProcessor>(const media::AudioFormat&)>;

/// @brief Observable lifecycle phase of a composition-owned microphone session.
enum class SessionState {
  /// Capture is inactive.
  Stopped,
  /// The permission check or device startup is pending, or the first valid PCM block has yet to arrive.
  Starting,
  /// The microphone has produced timestamped PCM and its format is available.
  Running,
  /// The run has stopped accepting PCM and the capture backend is releasing its resources.
  Stopping,
  /// Capture is requested while the application is in the background.
  Suspended,
  /// Startup or capture failed. Inspect CaptureStatus::error before retrying.
  Failed,
  /// The owning composition unmounted. Retained handles cannot start another run.
  Closed,
};

/// @brief A snapshot of the session phase, PCM format, capture failure, and recording pause.
struct CaptureStatus {
  /// Last published lifecycle phase.
  SessionState state = SessionState::Stopped;
  /// PCM format for the current run. Available in Running and retained during Stopping if PCM was observed.
  std::optional<media::AudioFormat> format;
  /// Present when state is Failed. Recording export failures are delivered by their Tasks.
  std::optional<CaptureError> error;
  /// True while the current recording skips incoming PCM during a pause.
  bool recording_paused = false;

  /// Compares all snapshot fields.
  bool operator==(const CaptureStatus&) const = default;
};

/// @brief Peak and RMS input levels over the latest measurement window.
struct AudioInputLevel {
  struct Channel {
    /// Largest sample magnitude, normalized from silence (0) to full scale (1).
    float peak = 0.0F;
    /// Root mean square sample magnitude on the same scale.
    float rms = 0.0F;

    bool operator==(const Channel&) const = default;
  };

  /// Number of measured channels, one for mono or two for stereo.
  std::uint32_t channel_count = 0;
  /// The first channel_count entries follow PCM channel order.
  std::array<Channel, 2> channels{};

  bool operator==(const AudioInputLevel&) const = default;
};

/// @brief A selectable microphone input reported by the platform.
/// @see EnumerateInputDevicesAsync(), CaptureOptions::device_id
struct InputDevice {
  /// Opaque UTF-8 identifier for CaptureOptions::device_id. The platform may assign a new ID after reconnecting.
  std::string id;
  /// Display name reported by the platform, or a label supplied by the capture backend.
  std::string name;

  /// Compares the identifier and display name.
  bool operator==(const InputDevice&) const = default;
};

/// @brief Declarative microphone intent, input selection, and PCM requirements supplied to UseAudioCapture().
///
/// Changing the device, sample rate, channel layout, or processor factory while active remains true ends
/// the current run and starts a new one. Readers of the previous run drain queued PCM before EOF.
struct CaptureOptions {
  /// Request capture in the foreground. On Android, iOS, macOS, and Windows obtain Permission::Microphone first.
  /// On Linux and Web enable capture from the user's start action and observe the result in CaptureStatus.
  /// Clearing the intent stops input and lets the reader drain queued PCM.
  bool active = false;

  /// @brief Required PCM sample rate in samples per second per channel.
  ///
  /// std::nullopt uses the platform's default capture rate. A supplied rate must be positive and fit
  /// in std::int32_t. The input or platform converter must deliver that rate for the run to start.
  /// Inspect CaptureStatus::format for the actual PCM format and CaptureStatus::error for startup failure.
  std::optional<std::uint32_t> sample_rate;

  /// @brief Required PCM channel layout. Set Mono for one channel or Stereo for two ordered channels.
  ///
  /// std::nullopt uses the platform's default capture layout. An explicit layout requires matching PCM
  /// from the input or platform converter. Stereo describes the output layout even when the platform
  /// derives both channels from a mono input. Inspect CaptureStatus::format for the delivered layout.
  std::optional<media::AudioChannelLayout> channel_layout;

  /// @brief Required input device ID, as returned by EnumerateInputDevicesAsync().
  ///
  /// std::nullopt uses the platform default at the start of each run. An empty ID is invalid. If the requested
  /// input cannot be selected, the run fails with Unavailable. Loss of the input during capture ends the run.
  std::optional<std::string> device_id;

  /// Measures microphone input levels. This option can change during an active run.
  bool monitor_input_level = false;

  /// Optional factory for processing PCM delivered by the reader, including reads by RecordAsync().
  /// Reuse the same shared factory across compositions to keep the current run.
  std::shared_ptr<const PcmProcessorFactory> pcm_processor;

  /// Compares all capture options.
  bool operator==(const CaptureOptions&) const = default;
};

namespace detail {
class CaptureRun;
class ProcessedReader;
struct Session;
} // namespace detail

/// @brief Move-only PCM reader bound to one capture run.
///
/// Consume PCM with ReadAsync() or move the reader to a worker for blocking reads on desktop and mobile.
/// The queue holds up to 128 unread blocks and 512 KiB of PCM.
/// Returning a block releases its queue reservation. Retaining its sample storage does not count against
/// the queue. If the queue overflows, the run fails and discards unread blocks.
///
/// Keep reads serialized. FinishInput() may run concurrently with a read. Serialize moves and destruction with
/// all reader operations. Releasing the reader drops its queued PCM and allows another reader to attach while
/// the run is active. A restart creates a new run and requires a new reader.
/// A configured processor runs on PCM admitted to this reader. Capture-timed pending input is bounded by
/// the same block and byte limits as the capture queue.
/// @see AudioCaptureSession::OpenReader(), CapturedAudio
class AudioReader final {
public:
  /// Transfers the run's reader. Reads on the source then fail with NotReady.
  AudioReader(AudioReader&&) noexcept;
  /// Releases the current reader and takes ownership of the supplied reader.
  AudioReader& operator=(AudioReader&&) noexcept;
  /// Releases the reader and any queued PCM. The owning composition controls microphone capture.
  ~AudioReader();

  /// @brief Requests a graceful end to this reader's input while microphone capture continues.
  ///
  /// Without a cutoff, seals input immediately. With a cutoff, retains only complete sample frames ending
  /// at or before it, including in queued blocks. Input seals when captured PCM reaches or passes the cutoff
  /// or capture ends cleanly. The first accepted cutoff stays in effect when another cutoff is requested.
  /// A call without a cutoff can seal pending input immediately. Repeated calls after sealing have no effect.
  ///
  /// Reads continue through the retained PCM and processor output, then return a clean EOF. Capture failure
  /// before sealing is reported by the reader. Capture failure after sealing does not discard its remaining output.
  /// This call does not wait for PCM or drain the processor. Sealing wakes any pending read.
  /// @param capture_cutoff Optional time in the unprocessed PCM's capture clock domain and origin.
  /// @return Success when the request is accepted or input is already sealed. NotReady for a moved or released
  /// reader. Setting the first cutoff reports InvalidArgument if it has a zero timescale or precedes the end
  /// of PCM already dequeued. An existing capture failure is returned if input has not sealed.
  /// @see Read(), ReadAsync(), CapturedAudio::clock
  CaptureResult<void> FinishInput(std::optional<media::MediaTimestamp> capture_cutoff = std::nullopt);

  /// @brief Waits for the next block, the end of the reader's input, or a failure.
  ///
  /// A graceful stop drains queued blocks and processor output before returning EOF. A run failure before
  /// the reader seals input is returned as an error. Canceling a read leaves submitted PCM in the processor's state.
  /// @param cancellation Optional token for interrupting this wait independently of the capture run.
  /// @return An owned block on success, or a successful std::nullopt at EOF. Failure carries the run's
  /// error, Interrupted for a canceled read, NotReady after a move, or Unavailable on Web.
  /// @pre Call on a worker with no other read in progress.
  CaptureResult<std::optional<CapturedAudio>> Read(std::stop_token cancellation = {});

  /// @brief Waits for PCM, EOF, or a failure without blocking the UI thread.
  ///
  /// Canceling the Task withdraws the wait and suppresses its result. A raw block may already be dequeued.
  /// Submitted input and prepared output stay with the processor for the next read.
  /// Releasing the reader interrupts an outstanding wait and drops its unread queue.
  /// @return A Task yielding the next retained block, a successful std::nullopt at graceful EOF, or an error.
  /// An error can be the run's failure, ReaderInUse for an overlapping wait, Interrupted when the reader
  /// is released during the wait, or NotReady if it has already been moved from or released.
  /// @pre Await on the owning UI thread. Complete or cancel the current read before starting another read.
  Task<CaptureResult<std::optional<CapturedAudio>>> ReadAsync();

private:
  AudioReader(std::shared_ptr<detail::CaptureRun> run, std::uint64_t reader_id);
  void Release();
  std::shared_ptr<detail::CaptureRun> run_;
  std::shared_ptr<detail::ProcessedReader> processing_;
  std::uint64_t reader_id_ = 0;
  friend class detail::CaptureRun;
};

/// @brief PCM format and duration of a successfully finalized standalone recording.
struct RecordingSummary {
  /// Format of the PCM submitted to the writer.
  media::AudioFormat format;
  /// Exact duration in output samples per channel at the PCM sample rate.
  /// Paused samples, AAC encoder delay, and padding are excluded.
  media::MediaTimestamp duration;
};

/// @brief Copyable UI-thread handle to a microphone session owned by UseAudioCapture().
///
/// Copies refer to the same session. Unmount closes the session even if copies are retained. Access the
/// handle on its owning UI thread. Read PCM through the AudioReader returned by OpenReader().
/// @see UseAudioCapture(), CaptureStatus, AudioReader
class AudioCaptureSession final {
public:
  /// @brief Returns the current session snapshot.
  /// @return The last published capture and recording state. Reading it during composition observes later updates.
  CaptureStatus Status() const;

  /// @brief Observes microphone input levels for the current run on the owning UI thread.
  ///
  /// While monitoring is enabled, levels update about every 50 ms when PCM is available, including while
  /// recording is paused. Reading this during composition observes subsequent updates.
  /// @return The latest measurement, or std::nullopt until measured or when monitoring or capture stops.
  std::optional<AudioInputLevel> InputLevel() const;

  /// @brief Clears a failed state and retries with the current capture intent.
  ///
  /// Android, iOS, macOS, and Windows check the microphone grant. On Web the browser may prompt for access.
  /// Calling Retry() in any phase other than Failed has no effect.
  /// @pre Call on the owning UI thread outside composition.
  void Retry() const;

  /// @brief Attaches the sole PCM reader to a live run in Starting or Running.
  ///
  /// Only blocks admitted after attachment enter the reader's queue. A reader opened during Starting can
  /// wait for PCM or receive the startup error. Destroy the reader before attaching another to the same run.
  /// @return A reader on success, ReaderInUse if one is attached, or NotReady if no run accepts a reader.
  /// @pre Call on the owning UI thread from a UI event or committed Lifecycle setup.
  CaptureResult<AudioReader> OpenReader() const;

  /// @brief Records captured PCM to a new file using the selected codec and container through Lib-Media.
  ///
  /// The Task attaches the session's sole reader when it starts. It reports ReaderInUse if a reader is
  /// already attached and NotReady if no run accepts a reader.
  /// CodecId::PcmS16Le writes PCM to Wave, Rf64, or Mov. CodecId::Aac encodes AAC-LC for Adts, Mp4 (.m4a),
  /// or Mov. AAC requires 44100 or 48000 Hz mono or stereo PCM and a supported encoder configuration.
  /// MP4 and MOV retain known AAC priming and padding as presentation trim.
  /// Export runs on a worker on desktop and mobile and on the browser event loop on Web. A graceful stop
  /// drains accepted samples and the encoder, finalizes the container, and closes the file, including while
  /// paused. A failure or cancellation can leave partial output. Cancellation suppresses result delivery.
  /// An unsupported codec reports Unavailable. An unsupported codec and container combination or an
  /// existing destination file reports OutputFailed.
  /// @param destination New file in an existing writable directory. Use a temporary file on Web.
  /// @param codec CodecId::PcmS16Le or CodecId::Aac.
  /// @param container Container syntax, selected independently of the file name.
  /// @param bitrate AAC target bits per second, with positive values up to INT32_MAX subject to encoder support.
  /// @return A Task yielding the recording summary, NoAudio for a clean stop without PCM, or the failure.
  /// @pre Launch through TaskScope on the owning UI thread from a UI event or committed Lifecycle setup.
  /// @see StopRecording(), CaptureOptions::active
  Task<CaptureResult<RecordingSummary>> RecordAsync(File destination,
      media::CodecId codec, media::ContainerFormat container, std::uint32_t bitrate = 128000) const;

  /// @brief Records to an empty writable random-access stream.
  /// @param destination Stream supporting writes at absolute offsets, transferred to the Task.
  /// @param codec CodecId::PcmS16Le or CodecId::Aac.
  /// @param container Container syntax to write.
  /// @param bitrate AAC target bits per second, used for CodecId::Aac.
  /// @see RecordAsync(File, media::CodecId, media::ContainerFormat, std::uint32_t)
  Task<CaptureResult<RecordingSummary>> RecordAsync(RandomAccessStream destination,
      media::CodecId codec, media::ContainerFormat container, std::uint32_t bitrate = 128000) const;

  /// @brief Records to a fresh sequential output stream.
  ///
  /// Sequential export supports CodecId::Aac with ContainerFormat::Adts. ADTS carries all encoded access units
  /// in order, including encoder delay and padding. A container requiring random writes reports OutputFailed.
  /// @param destination Sequential stream positioned at the beginning, transferred to the Task.
  /// @param codec CodecId::Aac.
  /// @param container ContainerFormat::Adts.
  /// @param bitrate AAC target bits per second.
  /// @see RecordAsync(File, media::CodecId, media::ContainerFormat, std::uint32_t)
  Task<CaptureResult<RecordingSummary>> RecordAsync(OutputStream destination,
      media::CodecId codec, media::ContainerFormat container, std::uint32_t bitrate = 128000) const;

  /// @brief Skips new PCM blocks for the current recording while keeping the microphone open.
  /// Already queued blocks are still written. Calling during finalization or without a recording has no effect.
  /// @pre Call on the owning UI thread outside composition.
  void PauseRecording() const;

  /// @brief Accepts subsequent PCM blocks into the same recording.
  /// The recording timeline advances only by retained samples. Calling during finalization or without a
  /// recording has no effect.
  /// @pre Call on the owning UI thread outside composition.
  void ResumeRecording() const;

  /// @brief Stops accepting PCM for the current recording while keeping microphone capture active.
  ///
  /// A running or paused recording drains accepted PCM, processor output, and the encoder before finalizing
  /// the destination. Await the original RecordAsync() task for the result before starting another recording.
  /// Input level monitoring continues if enabled. Calling again during finalization or without a recording
  /// has no effect, including when OpenReader() owns the reader.
  /// @pre Call on the owning UI thread outside composition.
  /// @see RecordAsync(File, media::CodecId, media::ContainerFormat, std::uint32_t), CaptureOptions::active
  void StopRecording() const;

private:
  explicit AudioCaptureSession(std::shared_ptr<detail::Session> data);
  std::shared_ptr<detail::Session> data_;
  friend AudioCaptureSession UseAudioCapture(CaptureOptions, const std::source_location&);
};

/// @brief Registers the platform device discovery and capture backend without opening the microphone.
/// @param context Application context supplied to the hook. Add Install to AppOptions::application_hooks once.
void Install(ApplicationContext& context);

/// @brief Queries the microphone inputs currently selectable by this application.
///
/// The list is a snapshot. An input may disappear or reject a requested PCM format before capture starts.
/// Obtain Permission::Microphone first on Android, iOS, macOS, and Windows. On iOS, discovery uses an active
/// capture session or briefly opens the microphone in a temporary session. That session stops before the
/// Task reports a result. Linux uses configured ALSA inputs.
/// On Web, the browser may prompt for access and open a temporary microphone track to expose device IDs.
/// The Task closes that track before reporting a result. Canceling the Task discards the result and releases
/// any temporary track as soon as the browser supplies it.
/// @return A Task yielding the available input devices, an empty list when none are exposed, or a failure.
/// @pre Launch from a user action through TaskScope after registering Install() in the application hooks.
Task<CaptureResult<std::vector<InputDevice>>> EnumerateInputDevicesAsync();

/// @brief Observes or updates the microphone session at the calling composition site.
///
/// Committed setup applies the capture intent. Each start checks access through the platform's permission
/// service or the capture operation.
/// Backgrounding interrupts the run. Returning to the foreground starts a new run if capture is still
/// requested. Unmount permanently closes the session and interrupts any live reader.
/// @param options Capture intent, device selection, and requested PCM format. See CaptureOptions::active for authorization.
/// @param location Call-site identity used to retain the session across recomposition.
/// @return A handle to the composition-owned session.
/// @pre Call during composition on the UI thread after registering Install() in the application hooks.
AudioCaptureSession UseAudioCapture(
    CaptureOptions options = {}, const std::source_location& location = std::source_location::current());

} // namespace huxerui::audio
