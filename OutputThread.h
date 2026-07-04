#ifndef __MM_OUTPUT_THREAD
#define __MM_OUTPUT_THREAD

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include "MMGlobals.h"

#define MM_NUM_OUTPUT_BUFFERS 4

class OutputThread {
 public:
  static int Start(HWND hdwnd);
  static void Halt(HWND hdwnd);
  static void OnSoundData();
  static void Pause(HWND hdwnd) { CleanUpDevice(hdwnd); }

  static HANDLE hEvent;

 protected:
  static int OpenDevice(HWND hdwnd);
  static void CleanUpDevice(HWND hdwnd);
  static void ConvertFromTargetFormat(short* src_data, UINT num_frames, BYTE* dst_data, WAVEFORMATEX* dst_format);

  static IAudioClient* audio_client;
  static IAudioRenderClient* render_client;
  static uintptr_t output_thread_handle;

  static short buf[MM_NUM_OUTPUT_BUFFERS][MM_SOUND_BUFFER_LEN];
  static WAVEFORMATEX device_format;
  static UINT32 buffer_frame_count;
};

#endif
