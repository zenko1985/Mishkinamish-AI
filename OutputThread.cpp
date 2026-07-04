#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <process.h>
#include "OutputThread.h"
#include "WorkerThread.h"

IAudioClient* OutputThread::audio_client = NULL;
IAudioRenderClient* OutputThread::render_client = NULL;
HANDLE OutputThread::hEvent = NULL;
uintptr_t OutputThread::output_thread_handle = NULL;
short OutputThread::buf[MM_NUM_OUTPUT_BUFFERS][MM_SOUND_BUFFER_LEN];
WAVEFORMATEX OutputThread::device_format = {0};
UINT32 OutputThread::buffer_frame_count = 0;

static volatile bool flag_ShutDownOutputThread = false;
static volatile bool flag_DoNotWriteBuffers = false;

unsigned __stdcall WaveOutThread(void* p) {
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  while (!flag_ShutDownOutputThread) {
    HANDLE waitHandles[1];
    waitHandles[0] = OutputThread::hEvent;

    DWORD waitResult = WaitForMultipleObjects(1, waitHandles, FALSE, 500);

    if (waitResult == WAIT_OBJECT_0) {
      if (!flag_ShutDownOutputThread)
        OutputThread::OnSoundData();
    }
  }

  CoUninitialize();
  return 0;
}

int OutputThread::Start(HWND hdwnd) {
  // Clean up any existing device
  if (audio_client) {
    CleanUpDevice(hdwnd);
  }

  // Create event if needed
  if (!hEvent) {
    hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!hEvent) {
      return 1;
    }
  }

  int result = OpenDevice(hdwnd);
  if (result != 0) {
    return result;
  }

  // Create thread if needed
  if (!output_thread_handle) {
    output_thread_handle = _beginthreadex(NULL, 0, WaveOutThread, 0, 0, NULL);
    if (!output_thread_handle) {
      CloseHandle(hEvent);
      hEvent = NULL;
      CleanUpDevice(hdwnd);
      return 1;
    }
  }

  return 0;
}

int OutputThread::OpenDevice(HWND hdwnd) {
  HRESULT hr;
  IMMDeviceEnumerator* pEnumerator = NULL;
  IMMDevice* pDev = NULL;

  hr = CoCreateInstance(
      __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

  if (FAILED(hr) || !pEnumerator) {
    SetWindowTextA(hdwnd, "Не удалось создать MMDeviceEnumerator!");
    return 1;
  }

  hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
  pEnumerator->Release();

  if (FAILED(hr) || !pDev) {
    SetWindowTextA(hdwnd, "Устройство воспроизведения не найдено!");
    return 1;
  }

  REFERENCE_TIME hnsBufferDuration = 1 * 10000;
  bool exclusive_mode = false;

#ifndef AUDCLNT_STREAMFLAGS_NOPERSIST
#define AUDCLNT_STREAMFLAGS_NOPERSIST 0x00040000
#endif

  // Try exclusive mode with device mix format
  IAudioClient* temp_client = NULL;
  hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&temp_client);
  if (SUCCEEDED(hr) && temp_client) {
    WAVEFORMATEX* pwfx = NULL;
    hr = temp_client->GetMixFormat(&pwfx);
    if (SUCCEEDED(hr) && pwfx) {
      hr = temp_client->IsFormatSupported(
          AUDCLNT_SHAREMODE_EXCLUSIVE, pwfx, NULL);
      if (SUCCEEDED(hr)) {
        hr = temp_client->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
            hnsBufferDuration,
            hnsBufferDuration,
            pwfx,
            NULL);
        if (SUCCEEDED(hr)) {
          exclusive_mode = true;
          audio_client = temp_client;
          device_format = *pwfx;
        }
      }
      CoTaskMemFree(pwfx);
    }
    if (!exclusive_mode) {
      temp_client->Release();
    }
  }

  if (!exclusive_mode) {
    hr = pDev->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audio_client);
    pDev->Release();

    if (FAILED(hr) || !audio_client) {
      SetWindowTextA(hdwnd, "Не удалось активировать IAudioClient!");
      return 1;
    }

    WAVEFORMATEX* pwfx = NULL;
    hr = audio_client->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
      SetWindowTextA(hdwnd, "Не удалось получить формат микшера!");
      audio_client->Release();
      audio_client = NULL;
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
      SetWindowTextA(hdwnd, "Не удалось инициализировать WASAPI render!");
      audio_client->Release();
      audio_client = NULL;
      return 1;
    }
  } else {
    pDev->Release();
  }

  hr = audio_client->GetBufferSize(&buffer_frame_count);
  if (FAILED(hr)) {
    SetWindowTextA(hdwnd, "Не удалось получить размер буфера!");
    audio_client->Release();
    audio_client = NULL;
    return 1;
  }

  hr = audio_client->GetService(__uuidof(IAudioRenderClient), (void**)&render_client);
  if (FAILED(hr) || !render_client) {
    SetWindowTextA(hdwnd, "Не удалось получить IAudioRenderClient!");
    audio_client->Release();
    audio_client = NULL;
    return 1;
  }

  hr = audio_client->SetEventHandle(hEvent);
  if (FAILED(hr)) {
    SetWindowTextA(hdwnd, "Не удалось установить event handle!");
    render_client->Release();
    render_client = NULL;
    audio_client->Release();
    audio_client = NULL;
    return 1;
  }

  BYTE* pData = NULL;
  hr = render_client->GetBuffer(buffer_frame_count, &pData);
  if (SUCCEEDED(hr)) {
    ZeroMemory(pData, buffer_frame_count * device_format.nBlockAlign);
    render_client->ReleaseBuffer(buffer_frame_count, AUDCLNT_BUFFERFLAGS_SILENT);
  }

  hr = audio_client->Start();
  if (FAILED(hr)) {
    SetWindowTextA(hdwnd, "Не удалось запустить render!");
    render_client->Release();
    render_client = NULL;
    audio_client->Release();
    audio_client = NULL;
    return 1;
  }

  return 0;
}

void OutputThread::ConvertFromTargetFormat(short* src_data, UINT num_frames, BYTE* dst_data, WAVEFORMATEX* dst_format) {
  int dst_channels = dst_format->nChannels;
  int dst_bits = dst_format->wBitsPerSample;
  bool is_float = false;

  if (dst_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
      dst_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    is_float = true;
  }

  for (UINT i = 0; i < num_frames; i++) {
    float sample = src_data[i] / 32768.0f;

    for (int ch = 0; ch < dst_channels; ch++) {
      if (is_float) {
        ((float*)dst_data)[i * dst_channels + ch] = sample;
      } else if (dst_bits == 32) {
        ((int*)dst_data)[i * dst_channels + ch] = (int)(sample * 2147483647.0f);
      } else if (dst_bits == 24) {
        unsigned char* p = &dst_data[(i * dst_channels + ch) * 3];
        int val = (int)(sample * 8388607.0f);
        p[0] = (unsigned char)(val >> 8);
        p[1] = (unsigned char)(val >> 16);
        p[2] = (unsigned char)(val >> 24);
      } else {
        dst_data[(i * dst_channels + ch) * 2] = (unsigned char)(src_data[i] & 0xFF);
        dst_data[(i * dst_channels + ch) * 2 + 1] = (unsigned char)((src_data[i] >> 8) & 0xFF);
      }
    }
  }
}

void OutputThread::OnSoundData() {
  if (!render_client || flag_DoNotWriteBuffers) return;

  UINT32 num_frames_padding = 0;
  HRESULT hr = audio_client->GetCurrentPadding(&num_frames_padding);
  if (FAILED(hr)) return;

  UINT32 num_frames_available = buffer_frame_count - num_frames_padding;
  if (num_frames_available == 0) return;

  BYTE* pData = NULL;
  hr = render_client->GetBuffer(num_frames_available, &pData);
  if (FAILED(hr)) return;

  short temp_buf[MM_SOUND_BUFFER_LEN];
  UINT frames_to_write = num_frames_available;
  if (frames_to_write > MM_SOUND_BUFFER_LEN) frames_to_write = MM_SOUND_BUFFER_LEN;

  if (WorkerThread::PullData(temp_buf)) {
    ZeroMemory(pData, num_frames_available * device_format.nBlockAlign);
    render_client->ReleaseBuffer(num_frames_available, AUDCLNT_BUFFERFLAGS_SILENT);
    return;
  }

  if (device_format.nChannels == 1 && device_format.wBitsPerSample == 16 &&
      device_format.wFormatTag == WAVE_FORMAT_PCM) {
    memcpy(pData, temp_buf, frames_to_write * sizeof(short));
    if (num_frames_available > frames_to_write) {
      ZeroMemory(pData + frames_to_write * sizeof(short),
                 (num_frames_available - frames_to_write) * sizeof(short));
    }
  } else {
    ConvertFromTargetFormat(temp_buf, frames_to_write, pData, &device_format);
    if (num_frames_available > frames_to_write) {
      UINT silence_frames = num_frames_available - frames_to_write;
      BYTE* pSilence = pData + frames_to_write * device_format.nBlockAlign;
      ZeroMemory(pSilence, silence_frames * device_format.nBlockAlign);
    }
  }

  render_client->ReleaseBuffer(num_frames_available, 0);
}

void OutputThread::CleanUpDevice(HWND hdwnd) {
  flag_DoNotWriteBuffers = true;

  if (audio_client) {
    audio_client->Stop();

    if (render_client) {
      render_client->Release();
      render_client = NULL;
    }

    audio_client->Release();
    audio_client = NULL;
  }

  flag_DoNotWriteBuffers = false;
}

void OutputThread::Halt(HWND hdwnd) {
  CleanUpDevice(hdwnd);

  flag_ShutDownOutputThread = true;
  if (hEvent) {
    SetEvent(hEvent);
  }

  if (output_thread_handle) {
    WaitForSingleObject((HANDLE)output_thread_handle, 1000);
    CloseHandle((HANDLE)output_thread_handle);
    output_thread_handle = NULL;
  }

  if (hEvent) {
    CloseHandle(hEvent);
    hEvent = NULL;
  }
}
