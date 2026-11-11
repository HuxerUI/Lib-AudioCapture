#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "capture_run.h"

using namespace huxerui::audio;
using namespace huxerui::media;

namespace {

std::mutex audio_session_mutex;
std::size_t active_captures = 0;
bool input_selected = false;

// Acquire and release the route while holding audio_session_mutex.
class CaptureRouteLease {
public:
  explicit CaptureRouteLease(bool selects_input) {
    if (input_selected || (selects_input && active_captures)) return;
    input_selected = selects_input;
    ++active_captures;
    acquired_ = true;
  }

  ~CaptureRouteLease() {
    if (!acquired_) return;
    input_selected = false;
    --active_captures;
  }

  explicit operator bool() const { return acquired_; }

private:
  bool acquired_ = false;
};

class RecordingCategory {
public:
  explicit RecordingCategory(AVAudioSession* audio) : audio_(audio) {}

  bool Prepare(NSError** error) {
    const auto category = audio_.category;
    if ([category isEqualToString:AVAudioSessionCategoryPlayAndRecord] ||
        [category isEqualToString:AVAudioSessionCategoryRecord] ||
        [category isEqualToString:AVAudioSessionCategoryMultiRoute]) return true;
    category_ = category;
    mode_ = audio_.mode;
    policy_ = audio_.routeSharingPolicy;
    options_ = audio_.categoryOptions;
    changed_ = [audio_ setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault
                routeSharingPolicy:AVAudioSessionRouteSharingPolicyDefault options:recording_options error:error];
    if (changed_) applied_options_ = audio_.categoryOptions;
    return changed_;
  }

  ~RecordingCategory() {
    if (changed_ && [audio_.category isEqualToString:AVAudioSessionCategoryPlayAndRecord] &&
        [audio_.mode isEqualToString:AVAudioSessionModeDefault] &&
        audio_.routeSharingPolicy == AVAudioSessionRouteSharingPolicyDefault && audio_.categoryOptions == applied_options_) {
      [audio_ setCategory:category_ mode:mode_ routeSharingPolicy:policy_ options:options_ error:nil];
    }
  }

private:
  static constexpr auto recording_options = AVAudioSessionCategoryOptionAllowBluetoothHFP |
                                            AVAudioSessionCategoryOptionMixWithOthers;
  AVAudioSession* audio_;
  NSString* category_ = nil;
  NSString* mode_ = nil;
  AVAudioSessionRouteSharingPolicy policy_ = AVAudioSessionRouteSharingPolicyDefault;
  AVAudioSessionCategoryOptions options_ = 0;
  AVAudioSessionCategoryOptions applied_options_ = 0;
  bool changed_ = false;
};

AVAudioSessionPortDescription* FindInput(NSArray<AVAudioSessionPortDescription*>* ports, NSString* id) {
  for (AVAudioSessionPortDescription* port in ports) {
    if ([port.UID isEqualToString:id]) return port;
  }
  return nil;
}

bool UsesInput(AVAudioSession* audio, NSString* id) {
  const auto inputs = audio.currentRoute.inputs;
  return inputs.count == 1 && [inputs.firstObject.UID isEqualToString:id];
}

std::optional<CaptureError> ConfigureMicrophoneSession(AVCaptureSession* session, AVCaptureDevice* device,
                                                       AVCaptureAudioDataOutput* output) {
  NSError* error = nil;
  AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
  if (!input) {
    const auto code = error.code == AVErrorApplicationIsNotAuthorizedToUseDevice
        ? CaptureErrorCode::PermissionDenied : CaptureErrorCode::CaptureFailed;
    return CaptureError{code, error.localizedDescription.UTF8String ?: "Cannot open the microphone."};
  }

  [session beginConfiguration];
  @try {
    if (![session canAddInput:input])
      return CaptureError{CaptureErrorCode::CaptureFailed, "Cannot connect the microphone input."};
    [session addInput:input];
    if (![session canAddOutput:output])
      return CaptureError{CaptureErrorCode::CaptureFailed, "Cannot configure microphone PCM output."};
    [session addOutput:output];
  } @finally {
    [session commitConfiguration];
  }
  return {};
}

std::vector<InputDevice> ReadInputDevices(AVAudioSession* audio) {
  std::vector<InputDevice> devices;
  for (AVAudioSessionPortDescription* port in audio.availableInputs) {
    const char* id = port.UID.UTF8String;
    if (!id || !*id) continue;
    const char* name = port.portName.UTF8String;
    devices.push_back({id, name && *name ? name : id});
  }
  return devices;
}

class PcmConverter {
public:
  PcmConverter(AudioFormat input, AudioFormat output) : input_format_(input), output_format_(output) {
    const auto source = DescribePcm(input);
    const auto destination = DescribePcm(output);
    if (AudioConverterNew(&source, &destination, &converter_) != noErr)
      throw CaptureError{CaptureErrorCode::Unavailable, "Cannot convert microphone PCM to the requested format."};

    OSStatus status = noErr;
    if (source.mChannelsPerFrame > destination.mChannelsPerFrame) {
      const UInt32 downmix = 1;
      const Float32 gains[]{0.5F, 0.5F};
      status = AudioConverterSetProperty(converter_, kAudioConverterPropertyPerformDownmix, sizeof(downmix), &downmix);
      if (status == noErr)
        status = AudioConverterSetProperty(converter_, kAudioConverterPropertyChannelMixMap, sizeof(gains), gains);
    } else if (source.mChannelsPerFrame < destination.mChannelsPerFrame) {
      const SInt32 map[]{0, 0};
      status = AudioConverterSetProperty(converter_, kAudioConverterChannelMap, sizeof(map), map);
    }
    if (status != noErr) {
      AudioConverterDispose(converter_);
      throw CaptureError{CaptureErrorCode::Unavailable, "Cannot configure the requested microphone channel layout."};
    }
  }

  PcmConverter(const PcmConverter&) = delete;
  PcmConverter& operator=(const PcmConverter&) = delete;
  ~PcmConverter() { AudioConverterDispose(converter_); }

  void Publish(huxerui::audio::detail::CaptureRun& run, CMTime input_pts, std::span<const std::int16_t> pcm) {
    const auto input_count = pcm.size() / input_format_.ChannelCount();
    const auto channels = output_format_.ChannelCount();
    const auto input_rate = input_format_.sample_rate;
    const auto output_rate = output_format_.sample_rate;
    if (pcm.size_bytes() > max_bytes ||
        input_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - input_frames_))
      throw CaptureError{CaptureErrorCode::CaptureFailed, "The microphone PCM block exceeds the conversion limit."};
    const auto input_end = input_frames_ + static_cast<std::int64_t>(input_count);
    const auto output_end = CMTimeConvertScale(CMTimeMake(input_end, input_rate), output_rate,
                                              kCMTimeRoundingMethod_RoundTowardPositiveInfinity);
    if (!CMTIME_IS_NUMERIC(output_end) || output_end.value < output_frames_)
      throw CaptureError{CaptureErrorCode::CaptureFailed, "Invalid converted microphone sample position."};
    const auto capacity = output_end.value - output_frames_;
    if (static_cast<std::uint64_t>(capacity) >= max_bytes / (channels * sizeof(std::int16_t)))
      throw CaptureError{CaptureErrorCode::CaptureFailed, "The converted microphone PCM exceeds the buffer limit."};

    // Leave room for another request so the converter can signal that it needs the next input block.
    auto count = static_cast<UInt32>(capacity + 1);
    output_.resize(static_cast<std::size_t>(count) * channels);
    pending_.assign(pcm.begin(), pcm.end());
    AudioBufferList output{1, {{channels, static_cast<UInt32>(output_.size() * sizeof(std::int16_t)), output_.data()}}};
    const auto status = AudioConverterFillComplexBuffer(converter_, SupplyInput, this, &count, &output, nullptr);
    if ((status != noErr && status != needs_input) || !pending_.empty() || static_cast<std::int64_t>(count) > capacity)
      throw CaptureError{CaptureErrorCode::CaptureFailed, "Cannot convert microphone PCM to the requested format."};

    // Resampling can read ahead. Map the first converted sample's position back to the input clock.
    const auto offset = CMTimeSubtract(CMTimeMake(output_frames_, output_rate), CMTimeMake(input_frames_, input_rate));
    const auto pts = CMTimeAdd(input_pts, offset);
    input_frames_ = input_end;
    if (!count) return;
    if (!CMTIME_IS_NUMERIC(pts) || pts.epoch != 0 || pts.timescale <= 0)
      throw CaptureError{CaptureErrorCode::CaptureFailed, "Cannot map the converted microphone timestamp to host time."};
    output_frames_ += count;
    run.Publish(output_format_, CaptureClock::CoreMediaHostTime, {pts.value, static_cast<std::uint32_t>(pts.timescale)},
                std::span(output_).first(static_cast<std::size_t>(count) * channels));
  }

private:
  static constexpr std::size_t max_bytes = 512 * 1024;
  static constexpr OSStatus needs_input = 1;

  static AudioStreamBasicDescription DescribePcm(AudioFormat format) {
    const auto channels = format.ChannelCount();
    const UInt32 bytes_per_frame = channels * sizeof(std::int16_t);
    return {
        .mSampleRate = static_cast<double>(format.sample_rate),
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = bytes_per_frame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = bytes_per_frame,
        .mChannelsPerFrame = channels,
        .mBitsPerChannel = 16,
    };
  }

  static OSStatus SupplyInput(AudioConverterRef, UInt32* count, AudioBufferList* data,
                              AudioStreamPacketDescription**, void* context) {
    auto& self = *static_cast<PcmConverter*>(context);
    const auto channels = self.input_format_.ChannelCount();
    *count = static_cast<UInt32>(self.pending_.size() / channels);
    if (!*count) return needs_input;
    // AudioConverter can read a supplied buffer until its next input callback.
    self.input_.swap(self.pending_);
    self.pending_.clear();
    *data = {1, {{channels, static_cast<UInt32>(self.input_.size() * sizeof(std::int16_t)), self.input_.data()}}};
    return noErr;
  }

  AudioFormat input_format_;
  AudioFormat output_format_;
  AudioConverterRef converter_ = nullptr;
  std::vector<std::int16_t> pending_;
  std::vector<std::int16_t> input_;
  std::vector<std::int16_t> output_;
  std::int64_t input_frames_ = 0;
  std::int64_t output_frames_ = 0;
};

struct SampleStorage {
  alignas(AudioBufferList) std::byte list[offsetof(AudioBufferList, mBuffers) + 2 * sizeof(AudioBuffer)];
  CMBlockBufferRef block = nullptr;

  ~SampleStorage() {
    if (block) CFRelease(block);
  }
};

std::int16_t ToS16(float sample) {
  if (!std::isfinite(sample)) return 0;
  return static_cast<std::int16_t>(std::clamp(sample * 32768.0F, -32768.0F, 32767.0F));
}

CMClockRef SessionClock(AVCaptureSession* session) {
  if (@available(iOS 15.4, *)) return session.synchronizationClock;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return session.masterClock;
#pragma clang diagnostic pop
}

} // namespace

@interface HUXIOSAudioCaptureOutput : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate> {
 @public
  AVCaptureSession* session_;
  NSString* input_id_;
  std::shared_ptr<huxerui::audio::detail::CaptureRun> run_;
  CMTime next_pts_;
  std::vector<std::int16_t> pcm_;
  std::optional<AudioFormat> source_format_;
  std::unique_ptr<PcmConverter> converter_;
}
@end

@implementation HUXIOSAudioCaptureOutput

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection {
  if (run_->StopToken().stop_requested()) return;
  try {
    if (input_id_ && !UsesInput(AVAudioSession.sharedInstance, input_id_)) {
      run_->End(CaptureError{CaptureErrorCode::Interrupted, "The selected microphone route changed."});
      return;
    }
    const auto pts = CMSampleBufferGetPresentationTimeStamp(sample);
    const auto clock = SessionClock(session_);
    if (!clock || !CMTIME_IS_NUMERIC(pts)) {
      run_->End(CaptureError{CaptureErrorCode::CaptureFailed, "The microphone did not provide a sample timestamp."});
      return;
    }
    const auto host_pts = CMSyncConvertTime(pts, clock, CMClockGetHostTimeClock());
    if (!CMTIME_IS_NUMERIC(host_pts) || host_pts.epoch != 0 || host_pts.timescale <= 0) {
      run_->End(CaptureError{CaptureErrorCode::CaptureFailed, "Cannot map the microphone timestamp to host time."});
      return;
    }

    const auto description = CMSampleBufferGetFormatDescription(sample);
    const auto* asbd = description ? CMAudioFormatDescriptionGetStreamBasicDescription(description) : nullptr;
    const auto count = CMSampleBufferGetNumSamples(sample);
    const bool valid = asbd && asbd->mFormatID == kAudioFormatLinearPCM &&
        (asbd->mFormatFlags & kAudioFormatFlagIsPacked) && !(asbd->mFormatFlags & kAudioFormatFlagIsBigEndian) &&
        (asbd->mChannelsPerFrame == 1 || asbd->mChannelsPerFrame == 2) && asbd->mSampleRate >= 1 &&
        asbd->mSampleRate <= std::numeric_limits<std::int32_t>::max() && std::floor(asbd->mSampleRate) == asbd->mSampleRate;
    const bool floating = valid && (asbd->mFormatFlags & kAudioFormatFlagIsFloat);
    if (!valid || (floating ? asbd->mBitsPerChannel != 32 : asbd->mBitsPerChannel != 16 ||
        !(asbd->mFormatFlags & kAudioFormatFlagIsSignedInteger))) {
      run_->End(CaptureError{CaptureErrorCode::Unavailable, "The microphone PCM format is not supported."});
      return;
    }
    if (count <= 0 || count > std::numeric_limits<std::uint32_t>::max()) {
      run_->End(CaptureError{CaptureErrorCode::CaptureFailed, "Invalid microphone sample count."});
      return;
    }

    const auto rate = static_cast<std::int32_t>(asbd->mSampleRate);
    if (CMTIME_IS_NUMERIC(next_pts_)) {
      const auto gap = CMTimeSubtract(pts, next_pts_);
      // Allow small hardware clock variations without concealing dropped capture buffers.
      if (pts.epoch != next_pts_.epoch || !CMTIME_IS_NUMERIC(gap) || std::abs(CMTimeGetSeconds(gap)) > 0.002) {
        run_->End(CaptureError{CaptureErrorCode::Discontinuity, "The microphone sample timeline has a gap or overlap."});
        return;
      }
    }
    const auto duration = CMSampleBufferGetDuration(sample);
    next_pts_ = CMTimeAdd(pts, CMTIME_IS_NUMERIC(duration) && duration.value > 0 ? duration : CMTimeMake(count, rate));

    SampleStorage storage;
    auto* list = reinterpret_cast<AudioBufferList*>(storage.list);
    const auto channels = asbd->mChannelsPerFrame;
    const bool planar = asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
    const auto plane_channels = planar ? 1u : channels;
    const auto bytes_per_sample = floating ? sizeof(float) : sizeof(std::int16_t);
    const auto size = static_cast<std::size_t>(count);
    if (asbd->mBytesPerFrame != bytes_per_sample * plane_channels ||
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sample, nullptr, list, sizeof(storage.list), kCFAllocatorDefault, kCFAllocatorDefault,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &storage.block) != noErr ||
        list->mNumberBuffers != (planar ? channels : 1u)) {
      run_->End(CaptureError{CaptureErrorCode::CaptureFailed, "Cannot read the microphone PCM planes."});
      return;
    }
    for (UInt32 plane = 0; plane < list->mNumberBuffers; ++plane) {
      const auto& buffer = list->mBuffers[plane];
      if (!buffer.mData || buffer.mNumberChannels != plane_channels ||
          buffer.mDataByteSize < size * bytes_per_sample * plane_channels) {
        run_->End(CaptureError{CaptureErrorCode::CaptureFailed, "The microphone PCM plane is incomplete."});
        return;
      }
    }
    std::span<const std::int16_t> pcm;
    if (!floating && !planar) {
      pcm = {static_cast<const std::int16_t*>(list->mBuffers[0].mData), size * channels};
    } else {
      pcm_.resize(size * channels);
      for (std::size_t index = 0; index < size; ++index) {
        for (UInt32 channel = 0; channel < channels; ++channel) {
          const auto* data = list->mBuffers[planar ? channel : 0].mData;
          const auto offset = planar ? index : index * channels + channel;
          pcm_[index * channels + channel] = floating ? ToS16(static_cast<const float*>(data)[offset])
                                                     : static_cast<const std::int16_t*>(data)[offset];
        }
      }
      pcm = pcm_;
    }
    const AudioFormat format{static_cast<std::uint32_t>(rate), AudioSampleFormat::S16, AudioPacking::Interleaved,
                             channels == 1 ? huxerui::media::AudioChannelLayout::Mono : huxerui::media::AudioChannelLayout::Stereo};
    if (source_format_ && *source_format_ != format) {
      run_->End(CaptureError{CaptureErrorCode::Discontinuity, "The microphone format changed."});
      return;
    }
    source_format_ = format;
    const auto requested_format = run_->RequestedFormat(format);
    if (requested_format != format) {
      if (!converter_) converter_ = std::make_unique<PcmConverter>(format, requested_format);
      converter_->Publish(*run_, host_pts, pcm);
    } else {
      run_->Publish(format, CaptureClock::CoreMediaHostTime,
                    {host_pts.value, static_cast<std::uint32_t>(host_pts.timescale)}, pcm);
    }
  } catch (CaptureError& error) {
    run_->End(std::move(error));
  } catch (const std::exception& error) {
    run_->End(CaptureError{CaptureErrorCode::CaptureFailed, error.what()});
  }
}

@end

namespace huxerui::audio::detail {

InputDevicesResult EnumeratePlatformInputDevices() {
  @autoreleasepool {
    @try {
      std::lock_guard lock(audio_session_mutex);
      AVAudioSession* audio = AVAudioSession.sharedInstance;
      if (active_captures) return InputDevicesResult::Success(ReadInputDevices(audio));

      AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
      if (!device) return InputDevicesResult::Success({});
      RecordingCategory category(audio);
      NSError* error = nil;
      if (!category.Prepare(&error)) {
        return InputDevicesResult::Failure({CaptureErrorCode::Unavailable,
            error.localizedDescription.UTF8String ?: "Cannot query iOS microphone inputs."});
      }
      // The capture session activates the app audio session while it owns the input.
      AVCaptureSession* discovery = [[AVCaptureSession alloc] init];
      discovery.automaticallyConfiguresApplicationAudioSession = NO;
      AVCaptureAudioDataOutput* output = [[AVCaptureAudioDataOutput alloc] init];
      if (auto failure = ConfigureMicrophoneSession(discovery, device, output))
        return InputDevicesResult::Failure(std::move(*failure));
      @try {
        [discovery startRunning];
        if (!discovery.isRunning || !audio.currentRoute.inputs.count) {
          return InputDevicesResult::Failure({CaptureErrorCode::Unavailable,
              "Cannot activate microphone input for device discovery."});
        }
        return InputDevicesResult::Success(ReadInputDevices(audio));
      } @finally {
        if (discovery.isRunning) [discovery stopRunning];
      }
    } @catch (NSException* exception) {
      return InputDevicesResult::Failure({CaptureErrorCode::CaptureFailed,
          exception.reason.UTF8String ?: "Cannot list microphone devices."});
    }
  }
}

std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run) {
  using namespace std::chrono_literals;
  @autoreleasepool {
    std::unique_lock lock(audio_session_mutex);
    if (run->StopToken().stop_requested()) return {};
    const auto& requested_id = run->Options().device_id;
    const CaptureRouteLease lease(requested_id.has_value());
    if (!lease)
      return CaptureError{CaptureErrorCode::Unavailable, "The iOS audio session is already capturing with a conflicting input selection."};
    NSString* input_id = nil;
    if (requested_id) {
      input_id = [[NSString alloc] initWithBytes:requested_id->data() length:requested_id->size()
                                      encoding:NSUTF8StringEncoding];
      if (!input_id) return CaptureError{CaptureErrorCode::Unavailable, "The requested microphone ID is invalid."};
    }
    AVAudioSession* audio = input_id ? AVAudioSession.sharedInstance : nil;
    RecordingCategory category(audio);
    AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
    if (!device) return CaptureError{CaptureErrorCode::Unavailable, "No default microphone is available."};

    AVCaptureSession* session = [[AVCaptureSession alloc] init];
    AVCaptureAudioDataOutput* output = [[AVCaptureAudioDataOutput alloc] init];
    HUXIOSAudioCaptureOutput* delegate = [[HUXIOSAudioCaptureOutput alloc] init];
    dispatch_queue_t queue = dispatch_queue_create("org.huxerui.lib.audiocapture.pcm", DISPATCH_QUEUE_SERIAL);
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    NSMutableArray* observers = [NSMutableArray array];
    const std::weak_ptr<CaptureRun> weak = run;
    NSString* previous_input = nil;
    bool changed_input = false;
    @try {
      NSError* error = nil;
      if (input_id) {
        if (!category.Prepare(&error)) {
          return CaptureError{CaptureErrorCode::Unavailable,
              error.localizedDescription.UTF8String ?: "Cannot prepare the iOS microphone route."};
        }
        session.automaticallyConfiguresApplicationAudioSession = NO;
        previous_input = audio.preferredInput.UID;
      }
      if (auto failure = ConfigureMicrophoneSession(session, device, output)) return failure;

      delegate->session_ = session;
      delegate->run_ = run;
      delegate->input_id_ = input_id;
      if (!input_id) [output setSampleBufferDelegate:delegate queue:queue];
      [observers addObject:[center addObserverForName:AVCaptureSessionRuntimeErrorNotification
          object:session queue:nil usingBlock:^(NSNotification* note) {
        if (auto current = weak.lock()) {
          NSError* failure = note.userInfo[AVCaptureSessionErrorKey];
          current->End(CaptureError{CaptureErrorCode::CaptureFailed,
              failure.localizedDescription.UTF8String ?: "The microphone session failed."});
        }
      }]];
      [observers addObject:[center addObserverForName:AVCaptureSessionWasInterruptedNotification
          object:session queue:nil usingBlock:^(NSNotification*) {
        if (auto current = weak.lock())
          current->End(CaptureError{CaptureErrorCode::Interrupted, "The microphone session was interrupted."});
      }]];
      [observers addObject:[center addObserverForName:AVCaptureDeviceWasDisconnectedNotification
          object:device queue:nil usingBlock:^(NSNotification*) {
        if (auto current = weak.lock())
          current->End(CaptureError{CaptureErrorCode::Interrupted, "The microphone was disconnected."});
      }]];
      if (run->StopToken().stop_requested()) return {};
      [session startRunning];
      if (input_id && session.isRunning && !run->StopToken().stop_requested()) {
        AVAudioSessionPortDescription* port = FindInput(audio.availableInputs, input_id);
        if (!port) return CaptureError{CaptureErrorCode::Unavailable, "The selected microphone is unavailable."};
        if (![audio setPreferredInput:port error:&error]) {
          return CaptureError{CaptureErrorCode::Unavailable,
              error.localizedDescription.UTF8String ?: "Cannot select the microphone route."};
        }
        changed_input = ![previous_input isEqualToString:input_id];
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!UsesInput(audio, input_id)) {
          if (run->StopToken().stop_requested()) return {};
          if (std::chrono::steady_clock::now() >= deadline)
            return CaptureError{CaptureErrorCode::Unavailable, "iOS could not route capture to the selected microphone."};
          std::this_thread::sleep_for(10ms);
        }
        [observers addObject:[center addObserverForName:AVAudioSessionRouteChangeNotification
            object:audio queue:nil usingBlock:^(NSNotification*) {
          if (auto current = weak.lock(); current && !UsesInput(audio, input_id)) {
            current->End(CaptureError{CaptureErrorCode::Interrupted, "The selected microphone route changed."});
          }
        }]];
        [output setSampleBufferDelegate:delegate queue:queue];
      }
      if (!session.isRunning)
        return CaptureError{CaptureErrorCode::CaptureFailed, "The microphone session did not start."};
      lock.unlock();
      if (!run->WaitForFirstFrame(3s))
        return CaptureError{CaptureErrorCode::CaptureFailed, "The microphone produced no timestamped PCM."};
      run->WaitForStop();
    } @catch (NSException* exception) {
      return CaptureError{CaptureErrorCode::CaptureFailed, exception.reason.UTF8String ?: "Microphone capture failed."};
    } @finally {
      if (!lock.owns_lock()) lock.lock();
      for (id observer in observers) [center removeObserver:observer];
      [output setSampleBufferDelegate:nil queue:nullptr];
      if (changed_input && [audio.preferredInput.UID isEqualToString:input_id]) {
        [audio setPreferredInput:FindInput(audio.availableInputs, previous_input) error:nil];
      }
      if (session.isRunning) [session stopRunning];
      dispatch_sync(queue, ^{});
    }
    return {};
  }
}

} // namespace huxerui::audio::detail
