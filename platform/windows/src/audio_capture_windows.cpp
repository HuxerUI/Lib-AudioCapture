#include <windows.h>
#include <initguid.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>
#include <functiondiscoverykeys_devpkey.h>

#include <limits>
#include <string_view>
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

struct DeviceProperty {
  PROPVARIANT value{};
  ~DeviceProperty() { PropVariantClear(&value); }
};

HRESULT OpenEnumerator(ComPtr<IMMDeviceEnumerator>& enumerator) {
  return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(enumerator.GetAddressOf()));
}

std::string ToUtf8(std::wstring_view text) {
  if (text.empty() || text.size() > std::numeric_limits<int>::max()) return {};
  const auto length = static_cast<int>(text.size());
  const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
  if (!count) return {};
  std::string result(count, '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), count, nullptr, nullptr))
    return {};
  return result;
}

std::wstring ToWide(std::string_view text) {
  if (text.empty() || text.size() > std::numeric_limits<int>::max()) return {};
  const auto length = static_cast<int>(text.size());
  const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
  if (!count) return {};
  std::wstring result(count, L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), count)) return {};
  return result;
}

CaptureError WasapiError(const char* action, HRESULT result) {
  auto code = CaptureErrorCode::CaptureFailed;
  if (result == E_ACCESSDENIED) code = CaptureErrorCode::PermissionDenied;
  else if (result == AUDCLNT_E_DEVICE_INVALIDATED) code = CaptureErrorCode::Interrupted;
  else if (result == AUDCLNT_E_UNSUPPORTED_FORMAT || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
    code = CaptureErrorCode::Unavailable;
  return {code, std::string(action) + " (HRESULT " + std::to_string(result) + ")."};
}

} // namespace

InputDevicesResult EnumeratePlatformInputDevices() {
  ComApartment apartment;
  if (FAILED(apartment.result))
    return InputDevicesResult::Failure(WasapiError("Cannot initialize COM for microphone discovery", apartment.result));
  ComPtr<IMMDeviceEnumerator> enumerator;
  auto hr = OpenEnumerator(enumerator);
  if (FAILED(hr)) return InputDevicesResult::Failure(WasapiError("Cannot open the microphone enumerator", hr));
  ComPtr<IMMDeviceCollection> endpoints;
  hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, endpoints.GetAddressOf());
  if (FAILED(hr)) return InputDevicesResult::Failure(WasapiError("Cannot list microphone devices", hr));
  UINT count = 0;
  hr = endpoints->GetCount(&count);
  if (FAILED(hr)) return InputDevicesResult::Failure(WasapiError("Cannot count microphone devices", hr));

  std::vector<InputDevice> devices;
  devices.reserve(count);
  for (UINT index = 0; index < count; ++index) {
    ComPtr<IMMDevice> device;
    hr = endpoints->Item(index, device.GetAddressOf());
    if (FAILED(hr)) return InputDevicesResult::Failure(WasapiError("Cannot read a microphone endpoint", hr));
    LPWSTR raw_id = nullptr;
    hr = device->GetId(&raw_id);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> device_id(raw_id, CoTaskMemFree);
    if (FAILED(hr)) return InputDevicesResult::Failure(WasapiError("Cannot read a microphone ID", hr));
    auto id = device_id ? ToUtf8(device_id.get()) : std::string{};
    if (id.empty()) continue;
    ComPtr<IPropertyStore> properties;
    DeviceProperty label;
    std::string name;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf())) &&
        SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &label.value)) &&
        label.value.vt == VT_LPWSTR && label.value.pwszVal) {
      name = ToUtf8(label.value.pwszVal);
    }
    if (name.empty()) name = id;
    devices.push_back({std::move(id), std::move(name)});
  }
  return InputDevicesResult::Success(std::move(devices));
}

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
  auto hr = OpenEnumerator(enumerator);
  if (FAILED(hr)) return WasapiError("Cannot open the microphone enumerator", hr);
  ComPtr<IMMDevice> device;
  const auto& id = run->Options().device_id;
  if (id) {
    const auto device_id = ToWide(*id);
    if (device_id.empty()) return CaptureError{CaptureErrorCode::Unavailable, "The requested microphone ID is invalid."};
    hr = enumerator->GetDevice(device_id.c_str(), device.GetAddressOf());
    if (FAILED(hr)) return WasapiError("Cannot find the selected microphone", hr);
    ComPtr<IMMEndpoint> endpoint;
    EDataFlow direction = eAll;
    DWORD state = 0;
    if (FAILED(device.As(&endpoint)) || FAILED(endpoint->GetDataFlow(&direction)) || direction != eCapture ||
        FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
      return CaptureError{CaptureErrorCode::Unavailable, "The selected microphone is unavailable."};
    }
  } else {
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.GetAddressOf());
    if (FAILED(hr)) return WasapiError("Cannot find the default microphone", hr);
  }
  ComPtr<IAudioClient> client;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                        reinterpret_cast<void**>(client.GetAddressOf()));
  if (FAILED(hr)) {
    auto error = WasapiError("Cannot open the microphone", hr);
    if (id && error.code == CaptureErrorCode::Interrupted) error.code = CaptureErrorCode::Unavailable;
    return error;
  }

  const auto pcm_format = run->RequestedFormat(mono_48k);
  const auto channels = static_cast<WORD>(pcm_format.ChannelCount());
  const auto frame_bytes = static_cast<WORD>(channels * sizeof(std::int16_t));
  if (pcm_format.sample_rate > std::numeric_limits<DWORD>::max() / frame_bytes)
    return CaptureError{CaptureErrorCode::Unavailable, "The requested microphone PCM byte rate is out of range."};
  WAVEFORMATEX format{
      .wFormatTag = WAVE_FORMAT_PCM,
      .nChannels = channels,
      .nSamplesPerSec = pcm_format.sample_rate,
      .nAvgBytesPerSec = pcm_format.sample_rate * frame_bytes,
      .nBlockAlign = frame_bytes,
      .wBitsPerSample = 16,
      .cbSize = 0,
  };
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
      0, 0, &format, nullptr);
  if (FAILED(hr)) return WasapiError("Cannot configure the requested S16 PCM format", hr);
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

      const auto sample_count = static_cast<std::size_t>(count) * channels;
      std::span<const std::int16_t> pcm;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        silence.resize(sample_count);
        pcm = silence;
      } else if (data) {
        pcm = {reinterpret_cast<const std::int16_t*>(data), sample_count};
      }
      const bool accepted = run->Publish(pcm_format, CaptureClock::QueryPerformanceCounter,
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
