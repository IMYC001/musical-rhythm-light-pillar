/*
 * setmute.exe —— 默认播放设备的静音 / 音量状态夹具（测试用，不做别的）。
 *
 * 用法:
 *   setmute.exe                 # 只打印当前状态
 *   setmute.exe <0|1> [音量]    # 设静音位 / 音量(0..1)
 *
 * 为什么要单独有这个工具：
 *   1) **还原**。tonetest.exe --unmute 会在播放期间临时解除静音，
 *      正常退出时还原 —— 但进程被强杀（脚本超时、后台子进程被回收）时
 *      来不及还原，设备就留在「已解除静音、音量 100%」的状态上。
 *      这时需要有个工具能把它按回去，而不是靠手点音量图标。
 *   2) **做对照实验**。「静音的设备上，WASAPI 环回还抓不抓得到声音」
 *      是个必须实测的事实问题（文档里的说法互相矛盾），
 *      而实验需要能可靠地把设备置成静音 —— 就是这里。
 *
 * 本机默认设备出厂状态是「静音 = 是，主音量 = 0.000」（虚拟声卡，没人听），
 * 所以做完实验请执行 `setmute.exe 1 0` 还原。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    fprintf(stderr, "[setmute] CoInitialize 失败\n");
    return 1;
  }
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* dev = nullptr;
  IAudioEndpointVolume* vol = nullptr;
  int rc = 0;

  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&en)) ||
      FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
      FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                           (void**)&vol))) {
    fprintf(stderr, "[setmute] 拿不到默认播放设备\n");
    rc = 1;
  } else {
    BOOL mute = FALSE;
    float level = 0.f;
    vol->GetMute(&mute);
    vol->GetMasterVolumeLevelScalar(&level);

    if (argc >= 2) {
      const BOOL wantMute = (atoi(argv[1]) != 0) ? TRUE : FALSE;
      vol->SetMute(wantMute, nullptr);
      if (argc >= 3) {
        float want = (float)atof(argv[2]);
        if (want < 0.f) want = 0.f;
        if (want > 1.f) want = 1.f;
        vol->SetMasterVolumeLevelScalar(want, nullptr);
      }
      vol->GetMute(&mute);
      vol->GetMasterVolumeLevelScalar(&level);
      printf("[setmute] 已设置 → 静音=%s 音量=%.3f\n",
             mute ? "是" : "否", level);
    } else {
      printf("[setmute] 当前状态 → 静音=%s 音量=%.3f\n",
             mute ? "是" : "否", level);
    }
  }

  if (vol) vol->Release();
  if (dev) dev->Release();
  if (en) en->Release();
  CoUninitialize();
  return rc;
}
