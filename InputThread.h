#ifndef __MM_INPUT_THREAD
#define __MM_INPUT_THREAD

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include "MMGlobals.h"

#define MM_NUM_INPUT_BUFFERS 2

class InputThread {
 public:
  static int Start(UINT device_num,
                   HWND hdwnd);
  static void Halt(HWND hdwnd);
  static void OnSoundData();

  static wchar_t* GetDeviceName(UINT device_num);
  static unsigned long GetNumDevices();

  static HANDLE hEvent;

 protected:
  static int OpenDevice(UINT device_num, HWND hdwnd);
  static void CleanUpDevice(HWND hdwnd);
  static HRESULT ConvertToTargetFormat(WAVEFORMATEX* src_format, short* src_data, UINT num_frames, short* dst_data, UINT* out_frames);

  static IAudioClient* audio_client;
  static IAudioCaptureClient* capture_client;
  static uintptr_t input_thread_handle;

  static short buf[MM_NUM_INPUT_BUFFERS][MM_SOUND_BUFFER_LEN];
  static WAVEFORMATEX device_format;
};

#endif
