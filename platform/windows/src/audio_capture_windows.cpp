#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <limits>
#include <utility>
#include <vector>

#include "capture_run.h"

namespace huxerui::audio::detail {
namespace {

using Microsoft::WRL::ComPtr;

struct ComApartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  ~ComApartment() {
    if (SUCCEEDED(result)) CoUninitialize();
  }
};

struct Event {
  HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  ~Event() {
    if (handle) CloseHandle(handle);
  }
};

struct RunningStream {
  IAudioClient* client;
  ~RunningStream() { client->Stop(); }
};

struct CapturePacket {
  IAudioCaptureClient* client;
  UINT32 frames;
  ~CapturePacket() {
    if (frames) client->ReleaseBuffer(frames);
  }
  HRESULT Release() { return client->ReleaseBuffer(std::exchange(frames, 0)); }
};

CaptureError WasapiError(const char* action, HRESULT result) {
  auto code = CaptureErrorCode::CaptureFailed;
  if (result == E_ACCESSDENIED) code = CaptureErrorCode::PermissionDenied;
  else if (result == AUDCLNT_E_DEVICE_INVALIDATED) code = CaptureErrorCode::Interrupted;
  else if (result == AUDCLNT_E_UNSUPPORTED_FORMAT || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
    code = CaptureErrorCode::Unavailable;
  return {code, std::string(action) + " (HRESULT " + std::to_string(result) + ")."};
}

} // namespace

std::optional<CaptureError> RunPlatformCapture(const std::shared_ptr<CaptureRun>& run) {
  const auto stop = run->StopToken();
  if (stop.stop_requested()) return {};
  ComApartment apartment;
  if (FAILED(apartment.result)) return WasapiError("Cannot initialize COM for audio capture", apartment.result);

  Event samples;
  Event stopping;
  if (!samples.handle || !stopping.handle)
    return CaptureError{CaptureErrorCode::CaptureFailed, "Cannot create WASAPI capture events."};
  const std::stop_callback cancel(stop, [&] { SetEvent(stopping.handle); });

  ComPtr<IMMDeviceEnumerator> enumerator;
  auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(enumerator.GetAddressOf()));
  if (FAILED(hr)) return WasapiError("Cannot enumerate microphones", hr);
  ComPtr<IMMDevice> device;
  hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.GetAddressOf());
  if (FAILED(hr)) return WasapiError("Cannot find the default microphone", hr);
  ComPtr<IAudioClient> client;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                        reinterpret_cast<void**>(client.GetAddressOf()));
  if (FAILED(hr)) return WasapiError("Cannot open the default microphone", hr);

  WAVEFORMATEX format{
      .wFormatTag = WAVE_FORMAT_PCM,
      .nChannels = 1,
      .nSamplesPerSec = mono_48k.sample_rate,
      .nAvgBytesPerSec = mono_48k.sample_rate * 2,
      .nBlockAlign = 2,
      .wBitsPerSample = 16,
      .cbSize = 0,
  };
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
      0, 0, &format, nullptr);
  if (FAILED(hr)) return WasapiError("Cannot configure mono 48 kHz microphone capture", hr);
  hr = client->SetEventHandle(samples.handle);
  if (FAILED(hr)) return WasapiError("Cannot register the WASAPI sample event", hr);

  ComPtr<IAudioCaptureClient> capture;
  hr = client->GetService(IID_PPV_ARGS(capture.GetAddressOf()));
  if (FAILED(hr)) return WasapiError("Cannot create the WASAPI capture client", hr);
  if (stop.stop_requested()) return {};
  hr = client->Start();
  if (FAILED(hr)) return WasapiError("Cannot start the microphone", hr);
  RunningStream started{client.Get()};

  const HANDLE events[]{stopping.handle, samples.handle};
  std::vector<std::int16_t> silence;
  bool published = false;
  while (!stop.stop_requested()) {
    const auto event = WaitForMultipleObjects(2, events, FALSE, 3000);
    if (event == WAIT_OBJECT_0) break;
    if (event == WAIT_TIMEOUT)
      return CaptureError{CaptureErrorCode::CaptureFailed, "The microphone stopped producing PCM."};
    if (event != WAIT_OBJECT_0 + 1)
      return WasapiError("Cannot wait for microphone samples", HRESULT_FROM_WIN32(GetLastError()));

    while (!stop.stop_requested()) {
      UINT32 available = 0;
      hr = capture->GetNextPacketSize(&available);
      if (FAILED(hr)) return WasapiError("Cannot query microphone samples", hr);
      if (!available) break;
      BYTE* data = nullptr;
      UINT32 count = 0;
      DWORD flags = 0;
      UINT64 qpc = 0;
      hr = capture->GetBuffer(&data, &count, &flags, nullptr, &qpc);
      if (FAILED(hr)) return WasapiError("Cannot read microphone samples", hr);
      if (!count) break;
      CapturePacket packet{capture.Get(), count};
      if (published && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))
        return CaptureError{CaptureErrorCode::Discontinuity, "WASAPI lost microphone samples."};
      if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || qpc > std::numeric_limits<std::int64_t>::max())
        return CaptureError{CaptureErrorCode::CaptureFailed, "WASAPI returned an invalid sample timestamp."};

      std::span<const std::int16_t> pcm;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        silence.resize(count);
        pcm = silence;
      } else if (data) {
        pcm = {reinterpret_cast<const std::int16_t*>(data), count};
      }
      const bool accepted = run->Publish(mono_48k, CaptureClock::QueryPerformanceCounter,
                                        {static_cast<std::int64_t>(qpc), 10000000}, pcm);
      hr = packet.Release();
      if (FAILED(hr)) return WasapiError("Cannot release microphone samples", hr);
      if (!accepted) break;
      published = true;
    }
  }
  return {};
}

} // namespace huxerui::audio::detail
