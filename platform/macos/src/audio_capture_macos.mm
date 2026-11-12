#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "capture_run.h"

using namespace huxerui::audio;
using namespace huxerui::media;

namespace {

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
  if (@available(macOS 12.3, *)) return session.synchronizationClock;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return session.masterClock;
#pragma clang diagnostic pop
}

} // namespace

@interface HUXMacOSAudioCaptureOutput : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate> {
 @public
  AVCaptureSession* session_;
  std::shared_ptr<huxerui::audio::detail::CaptureRun> run_;
  CMTime next_pts_;
  std::vector<std::int16_t> pcm_;
}
@end

@implementation HUXMacOSAudioCaptureOutput

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection {
  if (run_->StopToken().stop_requested()) return;
  try {
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
    const auto plane_count = planar ? channels : 1u;
    const auto plane_channels = planar ? 1u : channels;
    const auto bytes_per_sample = floating ? sizeof(float) : sizeof(std::int16_t);
    const auto size = static_cast<std::size_t>(count);
    // Core Media expects the list size for this layout, even when the storage can hold more buffers.
    const auto list_size = offsetof(AudioBufferList, mBuffers) + plane_count * sizeof(AudioBuffer);
    if (asbd->mBytesPerFrame != bytes_per_sample * plane_channels ||
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sample, nullptr, list, list_size, kCFAllocatorDefault, kCFAllocatorDefault,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &storage.block) != noErr ||
        list->mNumberBuffers != plane_count) {
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
    run_->Publish({static_cast<std::uint32_t>(rate), AudioSampleFormat::S16, AudioPacking::Interleaved,
                   channels == 1 ? huxerui::media::AudioChannelLayout::Mono : huxerui::media::AudioChannelLayout::Stereo},
                  CaptureClock::CoreMediaHostTime,
                  {host_pts.value, static_cast<std::uint32_t>(host_pts.timescale)}, pcm);
  } catch (const std::exception& error) {
    run_->End(CaptureError{CaptureErrorCode::CaptureFailed, error.what()});
  }
}

@end

namespace huxerui::audio::detail {

InputDevicesResult EnumeratePlatformInputDevices() {
  @autoreleasepool {
    @try {
      NSArray<AVCaptureDeviceType>* types;
      if (@available(macOS 14, *)) {
        types = @[AVCaptureDeviceTypeMicrophone, AVCaptureDeviceTypeExternal];
      } else {
        types = @[AVCaptureDeviceTypeBuiltInMicrophone, AVCaptureDeviceTypeExternalUnknown];
      }
      auto* discovery = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
          mediaType:AVMediaTypeAudio position:AVCaptureDevicePositionUnspecified];
      std::vector<InputDevice> devices;
      for (AVCaptureDevice* device in discovery.devices) {
        const char* id = device.uniqueID.UTF8String;
        if (!device.isConnected || !id || !*id) continue;
        const char* name = device.localizedName.UTF8String;
        devices.push_back({id, name && *name ? name : id});
      }
      return InputDevicesResult::Success(std::move(devices));
    } @catch (NSException* exception) {
      return InputDevicesResult::Failure({CaptureErrorCode::CaptureFailed,
          exception.reason.UTF8String ?: "Cannot list microphone devices."});
    }
  }
}

std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run) {
  @autoreleasepool {
    if (run->StopToken().stop_requested()) return {};
    const auto& requested_id = run->Options().device_id;
    AVCaptureDevice* device = nil;
    if (requested_id) {
      NSString* unique_id = [[NSString alloc] initWithBytes:requested_id->data() length:requested_id->size()
                                       encoding:NSUTF8StringEncoding];
      if (unique_id) device = [AVCaptureDevice deviceWithUniqueID:unique_id];
    } else {
      device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
    }
    if (!device.isConnected || ![device hasMediaType:AVMediaTypeAudio]) {
      return CaptureError{CaptureErrorCode::Unavailable,
          requested_id ? "The selected microphone is unavailable." : "No default microphone is available."};
    }
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (!input) {
      auto code = CaptureErrorCode::CaptureFailed;
      if (error.code == AVErrorApplicationIsNotAuthorizedToUseDevice) code = CaptureErrorCode::PermissionDenied;
      else if (requested_id && (error.code == AVErrorDeviceNotConnected || error.code == AVErrorDeviceWasDisconnected ||
                                error.code == AVErrorDeviceInUseByAnotherApplication)) code = CaptureErrorCode::Unavailable;
      return CaptureError{code, error.localizedDescription.UTF8String ?: "Cannot open the microphone."};
    }

    AVCaptureSession* session = [[AVCaptureSession alloc] init];
    AVCaptureAudioDataOutput* output = [[AVCaptureAudioDataOutput alloc] init];
    HUXMacOSAudioCaptureOutput* delegate = [[HUXMacOSAudioCaptureOutput alloc] init];
    dispatch_queue_t queue = dispatch_queue_create("org.huxerui.lib.audiocapture.pcm", DISPATCH_QUEUE_SERIAL);
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    NSMutableArray* observers = [NSMutableArray array];
    const std::weak_ptr<CaptureRun> weak = run;
    @try {
      const auto& options = run->Options();
      if (options.sample_rate || options.channel_layout) {
        const auto description = device.activeFormat.formatDescription;
        const auto* format = description ? CMAudioFormatDescriptionGetStreamBasicDescription(description) : nullptr;
        const auto layout = format && format->mChannelsPerFrame == 2
            ? huxerui::media::AudioChannelLayout::Stereo : huxerui::media::AudioChannelLayout::Mono;
        const auto channels = AudioFormat{.channel_layout = options.channel_layout.value_or(layout)}.ChannelCount();
        const double rate = options.sample_rate ? *options.sample_rate : (format ? format->mSampleRate : 0.0);
        NSMutableDictionary<NSString*, id>* settings = [@{
          AVFormatIDKey : @(kAudioFormatLinearPCM),
          AVNumberOfChannelsKey : @(channels),
          AVLinearPCMBitDepthKey : @16,
          AVLinearPCMIsFloatKey : @NO,
          AVLinearPCMIsBigEndianKey : @NO,
          AVLinearPCMIsNonInterleaved : @NO,
        } mutableCopy];
        if (std::isfinite(rate) && rate > 0) settings[AVSampleRateKey] = @(rate);
        output.audioSettings = settings;
      }
      [session beginConfiguration];
      const BOOL can_add_input = [session canAddInput:input];
      if (can_add_input) [session addInput:input];
      const BOOL can_add_output = can_add_input && [session canAddOutput:output];
      if (can_add_output) [session addOutput:output];
      [session commitConfiguration];
      if (!can_add_output)
        return CaptureError{CaptureErrorCode::CaptureFailed, "Cannot configure microphone PCM output."};

      delegate->session_ = session;
      delegate->run_ = run;
      [output setSampleBufferDelegate:delegate queue:queue];
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
      if (!session.isRunning || !run->WaitForFirstFrame(std::chrono::seconds(3)))
        return CaptureError{CaptureErrorCode::CaptureFailed, "The microphone produced no timestamped PCM."};
      run->WaitForStop();
    } @catch (NSException* exception) {
      return CaptureError{CaptureErrorCode::CaptureFailed, exception.reason.UTF8String ?: "Microphone capture failed."};
    } @finally {
      for (id observer in observers) [center removeObserver:observer];
      [output setSampleBufferDelegate:nil queue:nullptr];
      if (session.isRunning) [session stopRunning];
      dispatch_sync(queue, ^{});
    }
    return {};
  }
}

} // namespace huxerui::audio::detail
