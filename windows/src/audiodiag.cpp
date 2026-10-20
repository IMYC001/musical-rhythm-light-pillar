/*
 * 音频链路诊断：把「环回到底收到了什么」直接量出来。
 *
 * 输出：
 *   · 所有活动的播放设备（名字 + ID）
 *   · 默认播放设备的 音量 / 静音 状态
 *   · 用与主程序完全相同的方式开环回，抓 N 秒，打印原始采样峰值与 RMS
 *   · 顺带打印环回格式（采样率 / 声道 / 位深）
 *
 * 用法：audiodiag.exe <秒>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

template <class T>
static void safeRelease(T** p) {
  if (*p) {
    (*p)->Release();
    *p = nullptr;
  }
}

static void printW(const char* tag, const wchar_t* s) {
  char buf[512] = {};
  WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
  printf("%s%s\n", tag, buf);
}

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? atof(argv[1]) : 2.0;
  setvbuf(stdout, nullptr, _IONBF, 0);

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  IMMDeviceEnumerator* en = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en);
  if (FAILED(hr)) {
    printf("[x] CoCreateInstance(MMDeviceEnumerator) hr=0x%08lX\n", (unsigned long)hr);
    return 1;
  }

  IMMDeviceCollection* col = nullptr;
  hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
  UINT n = 0;
  if (SUCCEEDED(hr)) col->GetCount(&n);
  printf("[i] 活动播放设备数 = %u\n", n);
  for (UINT i = 0; i < n; i++) {
    IMMDevice* d = nullptr;
    if (FAILED(col->Item(i, &d))) continue;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        printW("    · ", pv.pwszVal);
      PropVariantClear(&pv);
      safeRelease(&ps);
    }
    LPWSTR id = nullptr;
    if (SUCCEEDED(d->GetId(&id))) {
      printW("      id=", id);
      CoTaskMemFree(id);
    }
    safeRelease(&d);
  }
  safeRelease(&col);

  IMMDevice* def = nullptr;
  hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &def);
  if (FAILED(hr)) {
    printf("[x] GetDefaultAudioEndpoint(eRender,eConsole) hr=0x%08lX\n", (unsigned long)hr);
    return 1;
  }
  printf("[i] 默认播放设备已取到\n");

  {
    IAudioEndpointVolume* vol = nullptr;
    if (SUCCEEDED(def->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                (void**)&vol))) {
      BOOL mute = FALSE;
      float scalar = 0.0f;
      vol->GetMute(&mute);
      vol->GetMasterVolumeLevelScalar(&scalar);
      printf("[i] 主音量 = %.3f  静音 = %s\n", scalar, mute ? "是" : "否");
      safeRelease(&vol);
    }
  }

  /* 环回初始化，参数与 audio.cpp 完全一致 */
  IAudioClient* ac = nullptr;
  hr = def->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
  if (FAILED(hr)) {
    printf("[x] Activate(IAudioClient) hr=0x%08lX\n", (unsigned long)hr);
    return 1;
  }
  WAVEFORMATEX* wf = nullptr;
  ac->GetMixFormat(&wf);
  printf("[i] 混音格式 = %lu Hz, %u ch, %u bit\n", (unsigned long)wf->nSamplesPerSec,
         (unsigned)wf->nChannels, (unsigned)wf->wBitsPerSample);
  REFERENCE_TIME dur = (REFERENCE_TIME)(10000000.0 / 100.0);  // 100ms
  hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, dur, 0,
                      wf, nullptr);
  printf("[i] Initialize(LOOPBACK) hr=0x%08lX\n", (unsigned long)hr);
  if (FAILED(hr)) return 1;

  IAudioCaptureClient* cap = nullptr;
  hr = ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
  if (FAILED(hr)) {
    printf("[x] GetService(IAudioCaptureClient) hr=0x%08lX\n", (unsigned long)hr);
    return 1;
  }
  ac->Start();
  printf("[i] 开始抓 %.1f 秒…\n", seconds);

  const DWORD t0 = GetTickCount();
  double peak = 0.0, sumSq = 0.0;
  long long count = 0;
  long long packets = 0, silentPackets = 0;
  double peakThisSecond = 0.0;
  while ((GetTickCount() - t0) < (DWORD)(seconds * 1000.0)) {
    UINT32 packet = 0;
    hr = cap->GetNextPacketSize(&packet);
    if (FAILED(hr)) break;
    if (packet == 0) {
      Sleep(5);
      continue;
    }
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
    if (FAILED(hr)) break;
    packets++;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
      silentPackets++;
    } else if (data && wf->wBitsPerSample == 32) {
      const float* f = (const float*)data;
      const int ch = wf->nChannels;
      for (UINT32 i = 0; i < frames; i++) {
        for (int c = 0; c < ch; c++) {
          const double v = f[i * ch + c];
          const double a = fabs(v);
          if (a > peak) peak = a;
          if (a > peakThisSecond) peakThisSecond = a;
          sumSq += v * v;
          count++;
        }
      }
    }
    cap->ReleaseBuffer(frames);
  }
  const double rms = count ? sqrt(sumSq / (double)count) : 0.0;
  printf("[i] 包数=%lld  其中标记静音=%lld  采样数=%lld\n", packets, silentPackets, count);
  printf("[i] 原始峰值 = %.6f    RMS = %.6f    (dBFS 峰值 %.1f)\n", peak, rms,
         peak > 0 ? 20.0 * log10(peak) : -999.0);

  ac->Stop();
  safeRelease(&cap);
  safeRelease(&ac);
  safeRelease(&def);
  safeRelease(&en);
  CoUninitialize();
  return 0;
}
