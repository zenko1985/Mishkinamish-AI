#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <process.h>
#include "InputThread.h"
#include "OutputThread.h"
#include "WAVLoader.h"
#include "WorkerThread.h"

IAudioClient* InputThread::audio_client = NULL;
IAudioCaptureClient* InputThread::capture_client = NULL;
HANDLE InputThread::hEvent = NULL;
uintptr_t InputThread::input_thread_handle = NULL;
short InputThread::buf[MM_NUM_INPUT_BUFFERS][MM_SOUND_BUFFER_LEN];
WAVEFORMATEX InputThread::device_format = {0};

static volatile bool flag_ShutDownInputThread = false;
static volatile bool flag_DoNotAddBuffers = false;

static IMMDevice* pDevice[32] = {0};
static UINT cached_num_devices = 0;
static wchar_t cached_device_names[32][256] = {0};
static bool devices_cached = false;

extern volatile bool flag_keep_silence;

bool f_reading_file = false;

static void CacheDevices() {
  IMMDeviceEnumerator* pEnumerator = NULL;
  IMMDeviceCollection* pCollection = NULL;

  HRESULT hr = CoCreateInstance(
      __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
  if (FAILED(hr) || !pEnumerator) {
    return;
  }

  hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
  if (SUCCEEDED(hr) && pCollection) {
    UINT count = 0;
    pCollection->GetCount(&count);
    if (count > 32) count = 32;

    for (UINT i = 0; i < count; i++) {
      if (pDevice[i]) pDevice[i]->Release();

      hr = pCollection->Item(i, &pDevice[i]);
      if (SUCCEEDED(hr) && pDevice[i]) {
        IPropertyStore* pProps = NULL;
        hr = pDevice[i]->OpenPropertyStore(STGM_READ, &pProps);
        if (SUCCEEDED(hr) && pProps) {
          PROPVARIANT varName;
          PropVariantInit(&varName);
          hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
          if (SUCCEEDED(hr)) {
            wcsncpy_s(cached_device_names[i], 256, varName.pwszVal, _TRUNCATE);
          }
          PropVariantClear(&varName);
          pProps->Release();
        }
      }
    }
    cached_num_devices = count;
    pCollection->Release();
  }

  pEnumerator->Release();
  devices_cached = true;
}

unsigned __stdcall WaveInThread(void* p) {
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  while (!flag_ShutDownInputThread) {
    HANDLE waitHandles[1];
    waitHandles[0] = InputThread::hEvent;

    DWORD waitResult = WaitForMultipleObjects(1, waitHandles, FALSE, 500);

    if (waitResult == WAIT_OBJECT_0) {
      if (!flag_ShutDownInputThread)
        InputThread::OnSoundData();
    }
  }

  CoUninitialize();
  return 0;
}

int InputThread::Start(UINT device_num, HWND hdwnd) {
  // Clean up any existing device
  if (audio_client) {
    CleanUpDevice(hdwnd);
  }

  flag_keep_silence = true;

  // Create event if needed
  if (!hEvent) {
    hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!hEvent) {
      return 1;
    }
  }

  int result = OpenDevice(device_num, hdwnd);
  if (result != 0) {
    // Don't close hEvent — the thread (if already running) may be waiting on it,
    // or we may reuse it on a future retry.
    return result;
  }

  // Create thread if needed
  if (!input_thread_handle) {
    input_thread_handle = _beginthreadex(NULL, 0, WaveInThread, 0, 0, NULL);
    if (!input_thread_handle) {
      CloseHandle(hEvent);
      hEvent = NULL;
      CleanUpDevice(hdwnd);
      return 1;
    }
  }

  return 0;
}

int InputThread::OpenDevice(UINT device_num, HWND hdwnd) {
  if (!devices_cached) {
    CacheDevices();
  }

  HRESULT hr;
  IMMDevice* pDev = NULL;

  if (device_num < cached_num_devices && pDevice[device_num]) {
    pDev = pDevice[device_num];
    pDev->AddRef();
  } else {
    IMMDeviceEnumerator* pEnumerator = NULL;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
      SetWindowTextA(hdwnd, "Не удалось создать MMDeviceEnumerator!");
      return 1;
    }

    hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDev);
    pEnumerator->Release();

    if (FAILED(hr) || !pDev) {
      SetWindowTextA(hdwnd, "Устройство записи не найдено!");
      return 1;
    }
  }

  REFERENCE_TIME hnsBufferDuration = 1 * 10000;
  bool exclusive_mode = false;

#ifndef AUDCLNT_STREAMFLAGS_NOPERSIST
#define AUDCLNT_STREAMFLAGS_NOPERSIST 0x00040000
#endif

  // 1. Try exclusive mode with 16kHz mono 16-bit PCM
  IAudioClient* temp_client = NULL;
  hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&temp_client);
  if (SUCCEEDED(hr) && temp_client) {
    WAVEFORMATEX wfxExclusive = {0};
    wfxExclusive.wFormatTag = WAVE_FORMAT_PCM;
    wfxExclusive.nChannels = 1;
    wfxExclusive.nSamplesPerSec = 16000;
    wfxExclusive.wBitsPerSample = 16;
    wfxExclusive.nBlockAlign = 2;
    wfxExclusive.nAvgBytesPerSec = 32000;

    hr = temp_client->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE, &wfxExclusive, NULL);
    if (SUCCEEDED(hr)) {
      hr = temp_client->Initialize(
          AUDCLNT_SHAREMODE_EXCLUSIVE,
          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
          hnsBufferDuration,
          hnsBufferDuration,
          &wfxExclusive,
          NULL);
      if (SUCCEEDED(hr)) {
        exclusive_mode = true;
        audio_client = temp_client;
        device_format = wfxExclusive;
      }
    }
    if (!exclusive_mode) {
      temp_client->Release();
    }
  }

  // 2. Fall back to shared mode if exclusive failed
  if (!exclusive_mode) {
    hr = pDev->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audio_client);
    if (FAILED(hr) || !audio_client) {
      pDev->Release();
      SetWindowTextA(hdwnd, "Не удалось активировать IAudioClient!");
      return 1;
    }

    WAVEFORMATEX* pwfx = NULL;
    hr = audio_client->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
      SetWindowTextA(hdwnd, "Не удалось получить формат микшера!");
      audio_client->Release();
      audio_client = NULL;
      pDev->Release();
      return 1;
    }

    device_format = *pwfx;

    hr = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
        hnsBufferDuration,
        0,
        pwfx,
        NULL);

    CoTaskMemFree(pwfx);

    if (FAILED(hr)) {
      SetWindowTextA(hdwnd, "Не удалось инициализировать WASAPI capture!");
      audio_client->Release();
      audio_client = NULL;
      pDev->Release();
      return 1;
    }
  }

  pDev->Release();

  hr = audio_client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client);
  if (FAILED(hr) || !capture_client) {
    SetWindowTextA(hdwnd, "Не удалось получить IAudioCaptureClient!");
    audio_client->Release();
    audio_client = NULL;
    return 1;
  }

  hr = audio_client->SetEventHandle(hEvent);
  if (FAILED(hr)) {
    SetWindowTextA(hdwnd, "Не удалось установить event handle!");
    capture_client->Release();
    capture_client = NULL;
    audio_client->Release();
    audio_client = NULL;
    return 1;
  }

  hr = audio_client->Start();
  if (FAILED(hr)) {
    SetWindowTextA(hdwnd, "Не удалось запустить capture!");
    capture_client->Release();
    capture_client = NULL;
    audio_client->Release();
    audio_client = NULL;
    return 1;
  }

  SetWindowTextA(hdwnd, exclusive_mode
      ? "WASAPI exclusive mode 16kHz/16bit/mono"
      : "WASAPI устройство записи инициализировано");

  return 0;
}

HRESULT InputThread::ConvertToTargetFormat(WAVEFORMATEX* src_format, short* src_data, UINT num_frames, short* dst_data, UINT* out_frames) {
  int src_channels = src_format->nChannels;
  int src_sample_rate = src_format->nSamplesPerSec;
  int src_bits = src_format->wBitsPerSample;

  const int TARGET_RATE = 16000;
  const int TARGET_CHANNELS = 1;
  const int TARGET_BITS = 16;

  if (src_channels == TARGET_CHANNELS && src_sample_rate == TARGET_RATE && src_bits == TARGET_BITS) {
    UINT copy = num_frames;
    if (copy > MM_SOUND_BUFFER_LEN) copy = MM_SOUND_BUFFER_LEN;
    memcpy(dst_data, src_data, copy * sizeof(short));
    *out_frames = copy;
    return S_OK;
  }

  static short temp_mono[4096];
  UINT mono_frames = num_frames;

  if (src_bits == 32) {
    if (src_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        src_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      float* src_float = (float*)src_data;
      for (UINT i = 0; i < num_frames; i++) {
        float sample = 0.0f;
        for (int ch = 0; ch < src_channels; ch++) {
          sample += src_float[i * src_channels + ch];
        }
        sample /= src_channels;
        int val = (int)(sample * 32767.0f);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        temp_mono[i] = (short)val;
      }
    } else {
      int* src_int = (int*)src_data;
      for (UINT i = 0; i < num_frames; i++) {
        long long sample = 0;
        for (int ch = 0; ch < src_channels; ch++) {
          sample += src_int[i * src_channels + ch];
        }
        sample /= src_channels;
        int val = (int)(sample >> 16);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        temp_mono[i] = (short)val;
      }
    }
  } else if (src_bits == 24) {
    unsigned char* src_bytes = (unsigned char*)src_data;
    for (UINT i = 0; i < num_frames; i++) {
      long long sample = 0;
      for (int ch = 0; ch < src_channels; ch++) {
        unsigned char* p = &src_bytes[(i * src_channels + ch) * 3];
        int val = (p[2] << 24) | (p[1] << 16) | (p[0] << 8);
        sample += val;
      }
      sample /= src_channels;
      int val = (int)(sample >> 16);
      if (val > 32767) val = 32767;
      if (val < -32768) val = -32768;
      temp_mono[i] = (short)val;
    }
  } else {
    for (UINT i = 0; i < num_frames; i++) {
      long long sample = 0;
      for (int ch = 0; ch < src_channels; ch++) {
        sample += src_data[i * src_channels + ch];
      }
      temp_mono[i] = (short)(sample / src_channels);
    }
  }

  // DC offset removal
  long long dc_sum = 0;
  for (UINT i = 0; i < mono_frames; i++) {
    dc_sum += temp_mono[i];
  }
  int dc_offset = (int)(dc_sum / mono_frames);
  for (UINT i = 0; i < mono_frames; i++) {
    temp_mono[i] -= dc_offset;
  }

  if (src_sample_rate == TARGET_RATE) {
    memcpy(dst_data, temp_mono, mono_frames * sizeof(short));
    *out_frames = mono_frames;
    return S_OK;
  }

  double ratio = (double)TARGET_RATE / (double)src_sample_rate;
  UINT out_count = 0;
  double src_pos = 0.0;

  while (src_pos < mono_frames - 1 && out_count < MM_SOUND_BUFFER_LEN) {
    int idx = (int)src_pos;
    double frac = src_pos - idx;

    if (idx > 0 && idx < mono_frames - 2) {
      // Cubic Hermite interpolation (Catmull-Rom)
      double p0 = temp_mono[idx - 1];
      double p1 = temp_mono[idx];
      double p2 = temp_mono[idx + 1];
      double p3 = temp_mono[idx + 2];
      double t = frac;
      double t2 = t * t;
      double t3 = t2 * t;
      dst_data[out_count] = (short)(0.5 * ((2.0 * p1) +
        (-p0 + p2) * t +
        (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
        (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3));
    } else {
      // Linear interpolation at edges
      double s0 = temp_mono[idx];
      double s1 = temp_mono[idx + 1];
      dst_data[out_count] = (short)(s0 + frac * (s1 - s0));
    }
    out_count++;
    src_pos += 1.0 / ratio;
  }

  if (out_count < MM_SOUND_BUFFER_LEN && src_pos < mono_frames) {
    dst_data[out_count] = temp_mono[(int)src_pos];
    out_count++;
  }

  *out_frames = out_count;
  return S_OK;
}

void InputThread::OnSoundData() {
  if (!capture_client || flag_DoNotAddBuffers) return;

  UINT32 num_frames_available = 0;
  HRESULT hr = capture_client->GetNextPacketSize(&num_frames_available);

  if (FAILED(hr)) return;

  while (num_frames_available > 0) {
    BYTE* pData = NULL;
    UINT32 numFramesAvailable;
    DWORD dwFlags;

    hr = capture_client->GetBuffer(&pData, &numFramesAvailable, &dwFlags, NULL, NULL);
    if (FAILED(hr)) break;

    if (!(dwFlags & AUDCLNT_BUFFERFLAGS_SILENT) && numFramesAvailable > 0) {
      short converted[MM_SOUND_BUFFER_LEN] = {0};
      UINT out_frames = 0;

      ConvertToTargetFormat(&device_format, (short*)pData, numFramesAvailable, converted, &out_frames);

      if (out_frames > MM_SOUND_BUFFER_LEN) out_frames = MM_SOUND_BUFFER_LEN;

      if (out_frames > 0) {
        // AGC — normalize level to target RMS
        static float agc_gain = 1.0f;
        const float TARGET_RMS = 5000.0f;
        double sum_sq = 0.0;
        for (UINT i = 0; i < out_frames; i++) {
          sum_sq += converted[i] * converted[i];
        }
        double rms = sqrt(sum_sq / out_frames);
        if (rms > 1.0) {
          float desired = (float)(TARGET_RMS / rms);
          float rate = (desired < agc_gain) ? 0.1f : 0.02f;
          agc_gain += (desired - agc_gain) * rate;
          if (agc_gain > 10.0f) agc_gain = 10.0f;
          if (agc_gain < 0.1f) agc_gain = 0.1f;
          for (UINT i = 0; i < out_frames; i++) {
            int val = (int)(converted[i] * agc_gain);
            if (val > 32767) val = 32767;
            if (val < -32768) val = -32768;
            converted[i] = (short)val;
          }
        }

        WorkerThread::PushData(converted);
      }
    }

    capture_client->ReleaseBuffer(numFramesAvailable);

    hr = capture_client->GetNextPacketSize(&num_frames_available);
    if (FAILED(hr)) break;
  }

}

void InputThread::CleanUpDevice(HWND hdwnd) {
  flag_DoNotAddBuffers = true;

  if (audio_client) {
    audio_client->Stop();

    if (capture_client) {
      capture_client->Release();
      capture_client = NULL;
    }

    audio_client->Release();
    audio_client = NULL;
  }

  flag_DoNotAddBuffers = false;
}

void InputThread::Halt(HWND hdwnd) {
  CleanUpDevice(hdwnd);

  flag_ShutDownInputThread = true;
  if (hEvent) {
    SetEvent(hEvent);
  }

  if (input_thread_handle) {
    WaitForSingleObject((HANDLE)input_thread_handle, 1000);
    CloseHandle((HANDLE)input_thread_handle);
    input_thread_handle = NULL;
  }

  if (hEvent) {
    CloseHandle(hEvent);
    hEvent = NULL;
  }

  for (UINT i = 0; i < cached_num_devices; i++) {
    if (pDevice[i]) {
      pDevice[i]->Release();
      pDevice[i] = NULL;
    }
  }
  devices_cached = false;
  cached_num_devices = 0;
}

unsigned long InputThread::GetNumDevices() {
  if (!devices_cached) {
    CacheDevices();
  }
  return cached_num_devices;
}

wchar_t* InputThread::GetDeviceName(UINT device_num) {
  if (!devices_cached) {
    CacheDevices();
  }
  if (device_num < cached_num_devices) {
    return cached_device_names[device_num];
  }
  return L"Unknown";
}
