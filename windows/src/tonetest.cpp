/*
 * 测试音源：往系统默认播放设备推一段「有节奏」的复合信号，用来验证
 * 「WASAPI 环回 → FFT → 十频段包络 → 光柱」这条链真的通。
 *
 * 为什么不能放一段恒定正弦：作品的电平映射默认是 perband（每段自适应），
 * 它拿本段最近 10 秒的滚动直方图取 20%/99% 分位当动态范围，输出的是
 * 「当前值落在自己动态范围里的位置」。**恒定信号没有动态 → 输出恒为 0**。
 * 所以测试信号必须有强弱的起伏，否则链子是好的也测不出来。
 *
 * 信号结构（模仿一段鼓点）：
 *   · 每 0.5 秒一记低频鼓：60Hz + 100Hz，指数衰减 —— 打 subBass / bass
 *   · 每 0.25 秒一记宽频噪声爆破，高通感 —— 打 presence / brilliance / air
 *   · 持续的 1kHz 颤音（4Hz 正弦调幅）—— 打 mid / energy
 *
 * 用法：tonetest.exe <秒>
 *
 * ⚠ 关于「设备静音时环回抓不到声音」—— **这是错的**，已实测排除：
 *   默认设备 静音=是 / 主音量=0.000 时，环回照样拿到完整信号
 *   （峰值 0.671，和解除静音时**一模一样**）。
 *   之前一度以为静音是原因，其实那次 audiodiag 报「包数=0」是因为
 *   **当时根本没人放声音** —— 环回端点在没有任何渲染流时就不产包。
 *   所以这里**不需要**任何「临时解除静音」的开关：那是无用的，
 *   而且会在进程被强杀时把用户的音量状态留在改过的样子上。
 *   真要检查/还原设备状态，用 setmute.exe（只做这一件事）。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma pack(push, 1)
struct WavHdr {
  char riff[4];
  unsigned size;
  char wave[4];
  char fmt[4];
  unsigned fmtSize;
  unsigned short audioFormat;
  unsigned short channels;
  unsigned sampleRate;
  unsigned byteRate;
  unsigned short blockAlign;
  unsigned short bitsPerSample;
  char data[4];
  unsigned dataSize;
};
#pragma pack(pop)

static const double PI = 3.14159265358979323846;

/* 简单 LCG，保证每次跑出来的噪声一样，报告可复现 */
static unsigned g_seed = 12345u;
static double frand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return ((g_seed >> 16) & 0x7fff) / 16383.5 - 1.0;   // -1 .. 1
}

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? atof(argv[1]) : 10.0;
  const int rate = 48000;
  const int n = (int)(rate * seconds);

  std::vector<short> pcm((size_t)n * 2);
  for (int i = 0; i < n; i++) {
    const double t = (double)i / rate;

    /* 低频鼓：0.5 秒一次，指数衰减 */
    const double kickPhase = fmod(t, 0.5);
    const double kickEnv = exp(-kickPhase * 12.0);
    const double kick = kickEnv * (0.80 * sin(2 * PI * 60.0 * t) +
                                   0.35 * sin(2 * PI * 100.0 * t));

    /* 高频爆破：0.25 秒一次，衰减更快 */
    const double hitPhase = fmod(t, 0.25);
    const double hitEnv = exp(-hitPhase * 55.0);
    const double hit = hitEnv * 0.45 * frand();

    /* 中频颤音 */
    const double trem = 0.35 + 0.35 * (0.5 + 0.5 * sin(2 * PI * 4.0 * t));
    const double mid = trem * 0.30 * sin(2 * PI * 1000.0 * t) +
                       trem * 0.15 * sin(2 * PI * 3000.0 * t);

    double v = kick + hit + mid;
    if (v > 1.0) v = 1.0;
    else if (v < -1.0) v = -1.0;
    const short s = (short)(v * 22000.0);
    pcm[(size_t)i * 2 + 0] = s;
    pcm[(size_t)i * 2 + 1] = s;
  }

  WavHdr h = {};
  memcpy(h.riff, "RIFF", 4);
  memcpy(h.wave, "WAVE", 4);
  memcpy(h.fmt, "fmt ", 4);
  memcpy(h.data, "data", 4);
  h.fmtSize = 16;
  h.audioFormat = 1;
  h.channels = 2;
  h.sampleRate = (unsigned)rate;
  h.bitsPerSample = 16;
  h.blockAlign = (unsigned short)(h.channels * h.bitsPerSample / 8);
  h.byteRate = h.sampleRate * h.blockAlign;
  h.dataSize = (unsigned)(pcm.size() * 2);
  h.size = 4 + 8 + h.fmtSize + 8 + h.dataSize;

  std::vector<unsigned char> buf(sizeof(WavHdr) + pcm.size() * 2);
  memcpy(buf.data(), &h, sizeof(WavHdr));
  memcpy(buf.data() + sizeof(WavHdr), pcm.data(), pcm.size() * 2);

  /* SND_MEMORY 要求缓冲区在整个播放期间有效 —— 所以 buf 得活到播放结束 */
  if (!PlaySoundA((LPCSTR)buf.data(), nullptr,
                  SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT)) {
    return 2;
  }
  Sleep((DWORD)(seconds * 1000.0));
  PlaySoundA(nullptr, nullptr, 0);
  return 0;
}
