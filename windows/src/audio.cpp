#include "audio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double LP_IDLE_ENERGY_THRESHOLD = 0.02;  // 作品里的 idleEnergyThreshold

/* ============================ 小工具 ============================ */

static inline double clampd(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* 迭代式基-2 Cooley–Tukey。N=2048、每秒 30 次，开销可以忽略，
   所以不做查表优化，保持可读。 */
static void fftRadix2(double* re, double* im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * M_PI / (double)len;
    const double wr = cos(ang), wi = sin(ang);
    const int half = len >> 1;
    for (int i = 0; i < n; i += len) {
      double cr = 1.0, ci = 0.0;
      for (int k = 0; k < half; k++) {
        const int a = i + k, b = a + half;
        const double ur = re[a], ui = im[a];
        const double vr = re[b] * cr - im[b] * ci;
        const double vi = re[b] * ci + im[b] * cr;
        re[a] = ur + vr;
        im[a] = ui + vi;
        re[b] = ur - vr;
        im[b] = ui - vi;
        const double ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

/* ============================ 构造 ============================ */

LpAudio::LpAudio() {
  /* Blackman 窗，和 Web Audio 的 AnalyserNode 用的一致。
     ⚠ 窗函数直接决定「段电平 → dB」的绝对刻度，但注意下面的 perband 映射
       是**自归一化**的（用该段自己的滚动分位当上下限），所以刻度整体平移
       不影响结果 —— 只有「频谱形状」和「段内动态范围」是敏感的。 */
  for (int n = 0; n < LP_FFT_SIZE; n++) {
    const double t = (double)n / (double)LP_FFT_SIZE;
    win_[n] = 0.42 - 0.5 * cos(2.0 * M_PI * t) + 0.08 * cos(4.0 * M_PI * t);
  }
  rpRing_.assign((size_t)LP_OUT_BINS * (size_t)rpWin_, 0);
  rpHist_.assign((size_t)LP_OUT_BINS * LP_RP_BINS, 0);
  for (int i = 0; i < 512; i++) {
    bucket_[i] = 0.0;
    prevBucket_[i] = 0.0;
  }
  for (int i = 0; i < LP_OUT_BINS; i++) {
    spectrum_[i] = 0.0;
    rawDb_[i] = -200.0;
    bandK0_[i] = 0;
    bandK1_[i] = 1;
    bandTilt_[i] = 0.0;
  }
}

LpAudio::~LpAudio() { stop(); }

/* ============================ 参数变化 ============================ */

/* 第 i 段的下边界频率（Hz）。逐字对应 bridge 的
     function bandEdge(i) {
       if (BAND_MODE === 'log') return BAND_LO_HZ * Math.pow(MAX_HZ / BAND_LO_HZ, i / CH_BINS);
       return MAX_HZ * i / CH_BINS;
     }
   ⚠ 别在这里另写一份「看起来一样」的公式：buildBands 和 rebuildTilt 必须
     取到**同一个**边界，否则切到等比之后频段划分变了、倾斜补偿量却没变。 */
double LpAudio::bandEdgeHz(double i) const {
  if (bandMode == 1) {   // log：30–16k 等比
    const double lo = LP_BAND_LO_HZ;
    return lo * pow(LP_MAX_HZ / lo, i / (double)LP_CH_BINS);
  }
  return LP_MAX_HZ * i / (double)LP_CH_BINS;   // linear：0–16k 等宽
}

void LpAudio::rebuildTilt() {
  for (int i = 0; i < LP_CH_BINS; i++) {
    /* ★ 参考频率取自**当前**频段划分（和原作品一致）。
       v6 之前这里写死了 linear 的 `LP_MAX_HZ * i / LP_CH_BINS` ——
       等比模式下每段的中心频率完全不同，补偿量却按等宽算，
       是第二个偏差（比「切了没反应」轻，但也是错的）。 */
    const double fc = std::max(0.5 * (bandEdgeHz(i) + bandEdgeHz(i + 1)), LP_TILT_REF_HZ);
    bandTilt_[i] = balance * LP_SPECTRAL_TILT * (log(fc / LP_TILT_REF_HZ) / log(2.0));
  }
}

/* 只做「改数值」时该做的重算：dB 窗口 + 倾斜表。
   ⚠ 千万别顺手 resetTrackers()：滚动直方图是「每段自己的最近 10 秒分位」，
     清空之后要重新攒满才准，期间 (bandDb-lo)/span 全落在无意义的区间里 ——
     用户拖动滑杆时看到的就是「推到底了也没反应」。 */
void LpAudio::rebuildTiltOnly() {
  lvMin_ = LP_LV_MIN0 - gainDb;
  lvMax_ = lvMin_ + widthDb;
  rebuildTilt();
}

/* `--bands` 诊断：把两种频率轴各自的频段划分打出来。
   目的是让「切等宽/等比」这件事**可被看见** —— 之前这一项在面板上完全没反应，
   而它影响的其实是「哪一段物理频率落到哪个分析频段里」：
     等宽：段 j 覆盖 250·j – 250·(j+1) Hz  → subBass ≈ 0–500 Hz
     等比：段 j 覆盖 30·(16000/30)^(j/64)  → subBass ≈ 30–38 Hz
   同一套「10 个分析频段」的桶下标是写死的，所以换个频率轴 = 换掉它们
   实际对应的物理频率，观感自然完全不同。 */
void LpAudio::dumpBands(FILE* f, double sampleRate) {
  if (!f) return;
  const double nyq = sampleRate / 2.0;
  const int fftBins = LP_FFT_SIZE / 2;          // AnalyserNode.frequencyBinCount
  const double binHz = nyq / (double)fftBins;

  /* 作品的 10 个频段在 512 桶域上的边界（闭区间），逐字对应 bundle 里
     `i<=4 ? h+=De : i<=12 ? d+=De : i<=24 ? m+=De : ...`。
     ⚠ 只在诊断里复述一遍，**不要**拿去重构 analyze512 —— 那段逻辑已经
       和原作品逐帧比对过（maxAbs 0.000e+00），动它就要重新验证一遍。 */
  static const struct { const char* name; int lo, hi; } kAn[] = {
      {"subBass", 0, 4},    {"bass", 5, 12},       {"lowMid", 13, 24},
      {"mid", 25, 45},      {"highMid", 46, 81},   {"presence", 82, 120},
      {"brilliance", 121, 180}, {"air", 181, 255},
  };

  fprintf(f, "[bands] 采样率 %.0f Hz　FFT %d 点　频域 bin 宽 %.3f Hz　"
             "每声道 %d 段　上限 %.0f Hz\n",
          sampleRate, LP_FFT_SIZE, binHz, LP_CH_BINS, LP_MAX_HZ);

  const int saved = bandMode;
  for (int m = 0; m < 2; m++) {
    bandMode = m;
    fprintf(f, "\n[bands] ===== 频率轴 = %s =====\n",
            m == 1 ? "等比 30–16k（log）" : "等宽 0–16k（linear）");
    /* 64 段的边界：打全了太长，打头 4 段 + 每 8 段一个 + 末段 */
    fprintf(f, "[bands] 每段边界(Hz)：");
    for (int j = 0; j < LP_CH_BINS; j++) {
      if (j < 4 || j % 8 == 0 || j == LP_CH_BINS - 1)
        fprintf(f, " 段%d=%.0f–%.0f", j, bandEdgeHz((double)j), bandEdgeHz((double)j + 1.0));
    }
    fprintf(f, "\n");
    fprintf(f, "[bands] 10 个分析频段实际落在什么频率上（512 桶域 → 每声道段号 → Hz）：\n");
    for (const auto& b : kAn) {
      /* 512 桶 n 对应 spectrum 下标 floor(n/4)，也就是每声道第 floor(n/4) 段 */
      const int cLo = b.lo / 4, cHi = b.hi / 4;
      fprintf(f, "[bands]   %-11s 桶%3d–%3d → 段%2d–%2d → %8.1f – %8.1f Hz\n",
              b.name, b.lo, b.hi, cLo, cHi,
              bandEdgeHz((double)cLo), bandEdgeHz((double)cHi + 1.0));
    }
  }
  bandMode = saved;
}

/* FNV-1a 64 位。用无符号累加 —— 有符号溢出是 UB，编译器有权把它优化成别的。 */
long long LpAudio::bandSignature() const {
  unsigned long long h = 1469598103934665603ULL;
  for (int i = 0; i < LP_CH_BINS; i++) {
    h = (h ^ (unsigned long long)(unsigned)bandK0_[i]) * 1099511628211ULL;
    h = (h ^ (unsigned long long)(unsigned)bandK1_[i]) * 1099511628211ULL;
  }
  return (long long)h;
}

void LpAudio::buildBands() {
  if (sampleRate_ <= 0) return;
  const double nyq = sampleRate_ / 2.0;
  const int fftBins = LP_FFT_SIZE / 2;         // AnalyserNode.frequencyBinCount
  const double binHz = nyq / (double)fftBins;

  for (int j = 0; j < LP_CH_BINS; j++) {
    const double e0 = bandEdgeHz((double)j);
    const double e1 = bandEdgeHz((double)j + 1.0);
    int k0 = (int)floor(e0 / binHz);
    int k1 = (int)floor(e1 / binHz);
    if (k1 <= k0) k1 = k0 + 1;
    if (k0 > fftBins - 1) k0 = fftBins - 1;
    if (k1 > fftBins) k1 = fftBins;
    bandK0_[j] = k0;
    bandK1_[j] = k1;
  }
  rebuildTilt();

  lvMin_ = LP_LV_MIN0 - gainDb;
  lvMax_ = lvMin_ + widthDb;
}

void LpAudio::resetTrackers() {
  /* 窗口长度可以调（界面上的「跟踪窗口」，秒）。
     ★ 固定按 **30Hz** 换算成帧数 —— 直方图挂在**频谱**那一层，
       而频谱固定跑 30Hz（= 原作品 `browser-bridge.js` 的 `PUSH_HZ = 30`）。
       所以「渲染帧率」怎么改都不会动到这里，也就不需要重新攒 10 秒。 */
  const int hz = 30;
  int want = (int)lround(rpWinSec * hz);
  if (want < hz) want = hz;
  if (want > hz * 30) want = hz * 30;
  rpWin_ = want;

  const size_t need = (size_t)LP_OUT_BINS * (size_t)rpWin_;
  if (rpRing_.size() != need) rpRing_.assign(need, (int16_t)0);
  else std::fill(rpRing_.begin(), rpRing_.end(), (int16_t)0);
  if (rpHist_.size() != (size_t)LP_OUT_BINS * LP_RP_BINS)
    rpHist_.assign((size_t)LP_OUT_BINS * LP_RP_BINS, (uint16_t)0);
  else std::fill(rpHist_.begin(), rpHist_.end(), (uint16_t)0);
  rpPos_ = 0;
  rpFrame_ = 0;
}

void LpAudio::paramsChanged() {
  buildBands();
  resetTrackers();
}

/* ============================ 捕获 ============================ */

bool LpAudio::start() {
  if (running_.load()) return true;
  quit_.store(false);
  th_ = std::thread([this] { captureLoop(); });
  /* 等线程把设备打开（成功或失败都会置 running_ 或 err_）*/
  for (int i = 0; i < 200 && !running_.load() && !err_; i++)
    Sleep(10);
  if (err_) {
    if (th_.joinable()) th_.join();
    return false;
  }
  return true;
}

void LpAudio::stop() {
  quit_.store(true);
  if (th_.joinable()) th_.join();
  running_.store(false);
}

void LpAudio::captureLoop() {
  /* COM 在线程里初始化。loopback 抓的是**默认渲染设备**（扬声器）的输出，
     所以不需要用户去选设备。 */
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    err_ = "CoInitializeEx 失败";
    return;
  }

  IMMDeviceEnumerator* enumerator = nullptr;
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
  if (FAILED(hr) || !enumerator) {
    err_ = "找不到音频设备枚举器";
    CoUninitialize();
    return;
  }

  IMMDevice* device = nullptr;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (FAILED(hr) || !device) {
    err_ = "没有默认播放设备";
    enumerator->Release();
    CoUninitialize();
    return;
  }

  IAudioClient* client = nullptr;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
  if (FAILED(hr) || !client) {
    err_ = "无法激活 IAudioClient";
    device->Release();
    enumerator->Release();
    CoUninitialize();
    return;
  }

  WAVEFORMATEX* wfx = nullptr;
  hr = client->GetMixFormat(&wfx);
  if (FAILED(hr) || !wfx) {
    err_ = "GetMixFormat 失败";
    client->Release();
    device->Release();
    enumerator->Release();
    CoUninitialize();
    return;
  }

  sampleRate_ = (double)wfx->nSamplesPerSec;
  channels_ = wfx->nChannels > 0 ? wfx->nChannels : 2;

  /* 环回：抓渲染端点的输出。1 秒缓冲足够，反正我们只取最近 2048 帧。 */
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                          10000000, 0, wfx, nullptr);
  if (FAILED(hr)) {
    err_ = "IAudioClient::Initialize 环回模式失败";
    CoTaskMemFree(wfx);
    client->Release();
    device->Release();
    enumerator->Release();
    CoUninitialize();
    return;
  }

  IAudioCaptureClient* capture = nullptr;
  hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
  if (FAILED(hr) || !capture) {
    err_ = "GetService(IAudioCaptureClient) 失败";
    CoTaskMemFree(wfx);
    client->Release();
    device->Release();
    enumerator->Release();
    CoUninitialize();
    return;
  }

  const bool isFloat = (wfx->wBitsPerSample == 32);
  const int bytesPerSample = wfx->wBitsPerSample / 8;

  hr = client->Start();
  if (FAILED(hr)) {
    err_ = "IAudioClient::Start 失败";
    capture->Release();
    CoTaskMemFree(wfx);
    client->Release();
    device->Release();
    enumerator->Release();
    CoUninitialize();
    return;
  }

  buildBands();
  resetTrackers();
  running_.store(true);

  std::vector<float> tmp;
  while (!quit_.load()) {
    UINT32 packet = 0;
    if (FAILED(capture->GetNextPacketSize(&packet))) break;
    if (packet == 0) {
      Sleep(8);
      continue;
    }
    while (packet > 0 && !quit_.load()) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

      const size_t samples = (size_t)frames * channels_;
      tmp.resize(samples);
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        std::fill(tmp.begin(), tmp.end(), 0.0f);
      } else if (isFloat) {
        memcpy(tmp.data(), data, samples * sizeof(float));
      } else {
        /* 非 float 的混音格式（少见，一般出现在独占/老驱动上）*/
        for (size_t i = 0; i < samples; i++) {
          if (bytesPerSample == 2) {
            tmp[i] = ((const int16_t*)data)[i] / 32768.0f;
          } else if (bytesPerSample == 4) {
            tmp[i] = ((const int32_t*)data)[i] / 2147483648.0f;
          } else {
            tmp[i] = 0.0f;
          }
        }
      }
      capture->ReleaseBuffer(frames);

      {
        std::lock_guard<std::mutex> lk(bufMu_);
        /* 只留最近一段，避免缓冲无限增长 */
        const size_t keep = (size_t)LP_FFT_SIZE * 4 * channels_;
        if (pending_.size() + samples > keep) {
          const size_t drop = pending_.size() + samples - keep;
          pending_.erase(pending_.begin(), pending_.begin() + drop);
        }
        pending_.insert(pending_.end(), tmp.begin(), tmp.end());
      }
      if (FAILED(capture->GetNextPacketSize(&packet))) break;
    }
  }

  client->Stop();
  capture->Release();
  CoTaskMemFree(wfx);
  client->Release();
  device->Release();
  enumerator->Release();
  CoUninitialize();
  running_.store(false);
}

/* ============================ ② 64 段电平 ============================ */

void LpAudio::computeSpectrum128() {
  /* 取最近 2048 帧（每声道分别做 FFT）
   *
   * ⚠⚠ 这里是整条链最容易写错的地方：**读完不能清空缓冲**。
   * 原作品的 AnalyserNode 是一个「随音频采样率连续滑动的 2048 点窗口」，
   * getByteFrequencyData 只是读，不消费。如果这里 clear 掉，那么每次读到的
   * 都只是「上一帧到现在」那点新数据（48kHz 下每帧约 800 个采样），
   * 永远凑不满 2048 —— FFT 输入全 0，频谱恒为 -100dB，光柱看着还在动
   * （空闲波浪在撑），但完全不响应声音。
   * 正确做法：读最后 2048 帧，然后把更早的丢掉，只留滑动窗口。 */
  static thread_local std::vector<float> left(LP_FFT_SIZE), right(LP_FFT_SIZE);
  {
    std::lock_guard<std::mutex> lk(bufMu_);
    const size_t frames = pending_.size() / (size_t)channels_;
    if (frames >= (size_t)LP_FFT_SIZE) {
      const size_t off = (frames - LP_FFT_SIZE) * (size_t)channels_;
      for (int i = 0; i < LP_FFT_SIZE; i++) {
        const float* f = &pending_[off + (size_t)i * channels_];
        left[i] = f[0];
        right[i] = channels_ > 1 ? f[1] : f[0];
      }
      pending_.erase(pending_.begin(), pending_.begin() + off);   // 只保留窗口
    } else {
      /* 还没攒够一个窗口（刚启动），本帧给静音，但缓冲要留着继续攒 */
      std::fill(left.begin(), left.end(), 0.0f);
      std::fill(right.begin(), right.end(), 0.0f);
    }
  }

  const double w = lvMax_ - lvMin_;
  const double rpBinW = (LP_RP_DBMAX - LP_RP_DBMIN) / LP_RP_BINS;

  for (int ch = 0; ch < 2; ch++) {
    const std::vector<float>& src = (ch == 0) ? left : right;
    for (int i = 0; i < LP_FFT_SIZE; i++) {
      fftRe_[i] = (double)src[i] * win_[i];
      fftIm_[i] = 0.0;
    }
    fftRadix2(fftRe_, fftIm_, LP_FFT_SIZE);

    const int offset = ch * LP_CH_BINS;
    for (int i = 0; i < LP_CH_BINS; i++) {
      const int k0 = bandK0_[i], k1 = bandK1_[i];
      const int cnt = k1 - k0;
      double power = 0.0;
      for (int k = k0; k < k1; k++) {
        /* Chromium 的 AnalyserNode 把 FFT 结果除以 fftSize 后再取 dB，
           这里跟着除，保证 dB 的绝对刻度一致。 */
        const double mag = sqrt(fftRe_[k] * fftRe_[k] + fftIm_[k] * fftIm_[k]) /
                           (double)LP_FFT_SIZE;
        double db = 20.0 * log10(mag + 1e-30);
        /* 复刻 getByteFrequencyData 的量化 + 截断（DB_MIN..DB_MAX 之外会被夹住，
           这一步不能省 —— 作品的段电平是在夹过的值上继续算的）*/
        double norm = (db - LP_DB_MIN) / (LP_DB_MAX - LP_DB_MIN);
        norm = clampd(norm, 0.0, 1.0);
        const int byte = (int)lround(norm * 255.0);
        const double dbq = LP_DB_MIN + ((double)byte / 255.0) * (LP_DB_MAX - LP_DB_MIN);
        power += pow(10.0, dbq / 10.0);
      }
      if (cnt > 0) power /= (double)cnt;
      const double bandDb = 10.0 * log10(power + 1e-12);
      const int slot = offset + i;
      rawDb_[slot] = bandDb;

      if (mapMode == 0) {
        /* perband：把本帧电平丢进该段的滚动直方图，再从直方图读 p_lo / p_hi，
           得到「该段自己的动态范围」。

           ★ 这里**不需要任何抽稀**：整个 computeSpectrum128 已经固定跑 30Hz
             （原作品 `browser-bridge.js` 的 `PUSH_HZ = 30`），
             所以喂进直方图的样本天然就是 30Hz，窗口 300 帧 = 10 秒。
             （v5 曾经让这一层跟着渲染帧率跑、再靠抽稀折算回 30Hz —— 那是
               错的结构，见 tick() 的注释：原作品的数据层和分析层是两个频率。） */
        const size_t base = (size_t)slot * LP_RP_BINS;
        {
          int bi = (int)floor((bandDb - LP_RP_DBMIN) / rpBinW);
          if (bi < 0) bi = 0;
          else if (bi > LP_RP_BINS - 1) bi = LP_RP_BINS - 1;
          const size_t ridx = (size_t)slot * (size_t)rpWin_ + rpPos_;
          if (rpFrame_ >= rpWin_ && rpHist_[base + rpRing_[ridx]] > 0)
            rpHist_[base + rpRing_[ridx]]--;
          rpRing_[ridx] = (int16_t)bi;
          if (rpHist_[base + bi] < 65535) rpHist_[base + bi]++;
        }

        const double total = rpFrame_ >= rpWin_ ? rpWin_ : (rpFrame_ + 1);
        const double tlo = total * loPct, thi = total * hiPct;
        double acc = 0.0, lo = LP_RP_DBMIN, hi = LP_RP_DBMAX;
        bool gotLo = false;
        for (int k = 0; k < LP_RP_BINS; k++) {
          acc += rpHist_[base + k];
          if (!gotLo && acc >= tlo) {
            gotLo = true;
            lo = LP_RP_DBMIN + (k + 0.5) * rpBinW;
          }
          if (acc >= thi) {
            hi = LP_RP_DBMIN + (k + 0.5) * rpBinW;
            break;
          }
        }
        double span = hi - lo;
        if (span < minSpan) span = minSpan;
        double lin = (bandDb - lo) / span;
        lin = clampd(lin, 0.0, 1.0);
        spectrum_[slot] = pow(lin, contrast) * level;
      } else {
        double lv = (bandDb + bandTilt_[i] - lvMin_) / w;
        spectrum_[slot] = clampd(lv, 0.0, 1.0);
      }
    }
  }
  /* ★ 这里必须是 rpWin_ 而不是 LP_RP_WIN！
     LP_RP_WIN 是 300（默认窗口），而「跟踪窗口」可以调到别的秒数 →
     rpWin_ 可能不是 300。用 300 取模的话，环形缓冲只会在前 300 格上循环，
     后面的格子永远写不到，而直方图里的计数还在被加/减 ——
     分位会一路漂到错的地方，表现就是「跟踪窗口一调，画面就不对了」。
     这个 bug 在默认值（10 秒 / 30Hz → 恰好 300）下完全看不出来。 */
  rpPos_ = (rpPos_ + 1) % rpWin_;
  rpFrame_++;

  /* --dumpspec：把这一帧的 128 个频谱值落盘。
     放在函数最末尾、rpPos_ 推进之后，所以 dump 出来的每一行
     都严格对应一次 computeSpectrum128()，与直方图状态一一对齐。 */
  if (dump_) {
    fputs(dumpLabel_.c_str(), dump_);
    for (int i = 0; i < LP_OUT_BINS; i++) fprintf(dump_, ",%.9f", spectrum_[i]);
    fputc('\n', dump_);
    dumpFrames_++;
  }
}

/* ---- --dumpspec 的两个入口 ---- */

bool LpAudio::startDump(const wchar_t* path, const char* label) {
  stopDump();
  /* 用 _wfopen 而不是 fopen：dump 路径是命令行传进来的，
     可能带中文（"D:\我的测试\dump.csv"），fopen 会按 ANSI 解析而打不开。 */
  dump_ = _wfopen(path, L"wb");
  if (!dump_) return false;
  dumpLabel_ = label ? label : "spec";
  dumpFrames_ = 0;
  /* 头一行是注释，写明这一份 dump 是什么、按什么频率录的。
     同一个文件里可以 append 多段（不同 label），audiotest 按 label 分组。 */
  fprintf(dump_, "#spec128 label=%s hz=30 bins=%d\n", dumpLabel_.c_str(), LP_OUT_BINS);
  return true;
}

void LpAudio::stopDump() {
  if (dump_) {
    fclose(dump_);
    dump_ = nullptr;
  }
}

/* ============================ ③④ 分析链 ============================ */

void LpAudio::analyze512() {
  /* ③ 128 → 512：最近邻上采样，每个输入值占 4 个输出桶
        （对应作品里的 Math.floor(n * t / 512)）*/
  for (int n = 0; n < 512; n++) {
    const int idx = (int)floor((double)n * LP_OUT_BINS / 512.0);
    bucket_[n] = spectrum_[idx < LP_OUT_BINS ? idx : LP_OUT_BINS - 1];
  }

  /* ④ 作品 bundle 里的 getAudioData，逐项照抄 */
  const int n = 512;
  double r = 0, o = 0, C = 0;
  double h = 0, d = 0, m = 0, v = 0, y = 0, x = 0, b = 0, M = 0;

  for (int i = 0; i < n; i++) {
    const double De = bucket_[i];
    r += De;
    o += (double)i * De;
    C += fabs(De - prevBucket_[i]);
    if (i <= 4) h += De;
    else if (i <= 12) d += De;
    else if (i <= 24) m += De;
    else if (i <= 45) v += De;
    else if (i <= 81) y += De;
    else if (i <= 120) x += De;
    else if (i <= 180) b += De;
    else if (i <= 255) M += De;
    prevBucket_[i] = De;
  }

  const double R = r / n;
  /* ⚠ 分母比桶数大 1（h 有 5 桶却除以 5？不 —— 见下）：
     作品原文是 h/5、d/9、m/13、v/21、y/37、x/40、b/61、M/76，
     桶数分别是 5/8/12/21/36/39/60/75。也就是说**只有 h 是整除的**，
     其余一律多除了 1。这是原作者调过的刻度，照抄，不要「修正」。 */
  const double L = h / 5.0;
  const double I = d / 9.0;
  const double z = m / 13.0;
  const double U = v / 21.0;
  const double F = y / 37.0;
  const double P = x / 40.0;
  const double B = b / 61.0;
  const double W = M / 76.0;

  const double X = (h + d + m) / 27.0;
  const double J = (v + y) / 58.0;
  const double se = (x + b + M) / 177.0;
  const double ce = r > 0 ? (h + d + m + v) / r : 0.0;
  const double Q = r > 0 ? (x + b + M) / r : 0.0;
  const double Y = std::max(0.0, Q - prevBrightness_) * 10.0;
  prevBrightness_ = Q;
  const double K = std::max(0.0, 1.0 - C / n * 2.0);

  const double le = R * 1.5;
  int fe = 0;
  if (L > le) fe++;
  if (I > le) fe++;
  if (z > le) fe++;
  if (U > le) fe++;
  if (F > le) fe++;
  if (P > le) fe++;
  if (B > le) fe++;
  if (W > le) fe++;
  const double Re = fe / 8.0;
  const double q = r > 0 ? o / r : 0.0;

  /* ★★★ 一阶平滑系数：**必须用原作品的原始值，一次都不能换算**。
   *
   *  原作品 bundle 里（`getAudioData`）：
   *      const ee = r > 0 ? .15 : .08;
   *      this.smoothedData.subBass += (L - this.smoothedData.subBass) * ee;
   *      …（16 个量都是这一个系数）
   *  它是**每次调用作用一次**的固定系数，而且**没有**按 dt 缩放 ——
   *  同一个函数里的节拍检测器倒是按 dt 缩放了（1 - 0.6^(dt*60)），
   *  说明作者是有意这么写的。而它跑在 `requestAnimationFrame` 里，
   *  频率由 `FPS_LIMIT` 决定（browser-bridge.js 里默认 60）——
   *  作者自己也在注释里点明了这一点：
   *      「浏览器里不限制的话会跑满刷新率，节拍检测器的平滑系数会跟着变形」。
   *
   *  ⚠ v5 在这里犯过一个错：把 ee 当成「按 30Hz 推送、每次作用一次」，
   *    于是按相同时间常数换算成 ee_eff = 1-(1-0.15)^(30/pushHz)，
   *    在 60fps 下得到 0.0780。结果时间常数变成 0.214s，是原作品的 1.93 倍
   *    （原作品 0.111s）→ **包络峰值只剩 67%**，
   *    表现就是用户报的「响应幅度没有 Electron 大、部分节拍不响应」。
   *
   *  正确做法：分析层跟着渲染帧率跑，系数原样用 0.15 / 0.08。
   *  这样 fpsLimit=60 时与原作品（FPS_LIMIT=60）**逐帧一致**；
   *  把 fpsLimit 调到别的值，也就等价于把原作品的 FPS_LIMIT 调到那个值 ——
   *  包括它在高帧率下「更跟手、更抖」的那一面，那才是忠实复刻。 */
  const double ee = r > 0 ? 0.15 : 0.08;
  smooth_[0] += (X - smooth_[0]) * ee;    // bass
  smooth_[1] += (J - smooth_[1]) * ee;    // mid
  smooth_[2] += (se - smooth_[2]) * ee;   // treble
  smooth_[3] += (R - smooth_[3]) * ee;    // energy
  smooth_[4] += (L - smooth_[4]) * ee;    // subBass
  smooth_[5] += (z - smooth_[5]) * ee;    // lowMid
  smooth_[6] += (F - smooth_[6]) * ee;    // highMid
  smooth_[7] += (P - smooth_[7]) * ee;    // presence
  smooth_[8] += (B - smooth_[8]) * ee;    // brilliance
  smooth_[9] += (W - smooth_[9]) * ee;    // air
  smooth_[10] += (ce - smooth_[10]) * ee; // warmth
  smooth_[11] += (Q - smooth_[11]) * ee;  // brightness
  smooth_[12] += (Y - smooth_[12]) * ee;  // sharpness
  smooth_[13] += (K - smooth_[13]) * ee;  // smoothness
  smooth_[14] += (Re - smooth_[14]) * ee; // density
  smooth_[15] += (q - smooth_[15]) * ee;  // spectralCentroid

  bands.bass = (float)smooth_[0];
  bands.mid = (float)smooth_[1];
  bands.subBass = (float)smooth_[4];
  bands.lowMid = (float)smooth_[5];
  bands.highMid = (float)smooth_[6];
  bands.energy = (float)smooth_[3];
  bands.presence = (float)smooth_[7];
  bands.brilliance = (float)smooth_[8];
  bands.air = (float)smooth_[9];
  bands.brightness = (float)smooth_[11];

  /* 活跃 / 空闲判定，给空闲波浪用 */
  if (R > LP_IDLE_ENERGY_THRESHOLD) lastActiveTime_ = nowMs_;
  else lastIdleTime_ = nowMs_;
  if (!audioReceived_) {
    audioReceived_ = true;
    lastActiveTime_ = nowMs_;
    lastIdleTime_ = 0;
  }

  /* 电平表：和 bridge 的 updateMeter 同一套算式（sum/128*5.5，夹到 1）*/
  double sum = 0;
  for (int i = 0; i < LP_OUT_BINS; i++) sum += spectrum_[i];
  meterLevel = (float)std::min(1.0, (sum / LP_OUT_BINS) * 5.5);
}

/* ---- 测试钩子：把 128 个频谱值直接塞进来、跑一次分析 ----
 *
 *  为什么要有它：用户报「原生观感比 Electron 差、幅度偏小、部分节拍不响应」，
 *  这类问题靠肉眼和猜都定位不到。必须把**原生分析链**和**原作品 JS 分析链**
 *  喂**同一串输入**，逐帧比 16 个平滑量的输出，用数字说话。
 *
 *  注意它**只跑 ③④ 两段**（analyze512），不碰 FFT、不碰直方图 ——
 *  因为要对比的是「128 个频谱值 → 16 个平滑量」这一段，
 *  而 128 个频谱值由 --dumpspec 从真实音乐里录下来，
 *  两边吃的是完全相同的输入。
 *
 *  ⚠ 调用方必须自己按正确的**调用频率**喂：fpsLimit=60 就每 1/60 秒调一次。
 *    因为 ee 是「每次调用作用一次」的系数，调用频率直接决定时间常数 ——
 *    这正是本轮要验证的那件事。 */
void LpAudio::testFeedSpectrum(const double* spec128) {
  for (int i = 0; i < LP_OUT_BINS; i++) spectrum_[i] = spec128[i];
  analyze512();
}

void LpAudio::tick(double dt) {
  nowMs_ += dt * 1000.0;

  /* ★★★ 这里有**三个不同的频率**，原作品就是这么分的，不能合并。
   *
   *  ① **数据层**：FFT → 每声道 64 段 → 128 个频谱值。固定 **30Hz**。
   *     出处：`browser-bridge.js` 的 `var PUSH_HZ = 30;`
   *     （注释写着「壁纸引擎约 30 次/秒」）。滚动直方图挂在这一层，
   *     所以它也必须 30Hz，窗口 300 帧 = 10 秒。
   *
   *  ② **分析层**：128 → 512 桶上采样 → 8 个频段和 → 谱通量 / 重心 →
   *     16 个量的一阶平滑。固定 **250Hz**（= anaHz）。
   *     出处：bundle 里 `ke = setInterval(ht, 1e3/1e3)`，`ht` 调 `getAudioData(yt)`。
   *     请求周期 1ms，Chromium 把嵌套定时器钳到 4ms → 实测 **249.6 次/秒**
   *     （用 --lp-rateprobe 在真实 Electron 里量的，见 tools/README-对照台.md）。
   *
   *  ③ **渲染层**：`general.fps`（桥里默认 60）驱动着色器 uniform。
   *     出处：bundle 里 `<Canvas frameloop="demand">` + `iG fpsLimit={e}`，
   *     而 `e` 来自 `applyGeneralProperties({fps})` → 所以渲染恰好是 fpsLimit。
   *
   *  ⚠ 三个错法，全踩过：
   *    · 把 ①② 都钉在 30Hz（v1~v4）→ 用户报「帧率调节没用，每个选项观感都一样」；
   *    · 把 ①② 都挂在渲染帧率上、再把 ee 按 30Hz 做时间常数换算（v5）→
   *      分析层 60Hz 且 τ×1.93 → 幅度只剩 67% → 用户报「幅度没有 Electron 大」；
   *    · 只修 ee 不修频率（v6 初版）→ 分析层仍是 60Hz，τ=111ms，
   *      而桌面版实测是 250Hz、τ=24.6ms → **仍然慢 4.5 倍**，
   *      幅度偏小 + 弱拍被抹平的问题只解决了一部分。
   *    正确结构是**三层各按自己的频率**：数据 30Hz、分析 250Hz、渲染 fpsLimit。
   *
   *  ★ 关键：分析层**不跟渲染帧率走**，跟的是**真实时间**。
   *    在桌面版里那个定时器和渲染循环是两条独立的时钟（250Hz vs 60Hz），
   *    所以把 fpsLimit 调到 30 或 144 都**不该**改变包络的包络形状 ——
   *    只改变画面把它画出来的细腻程度。这也是为什么这里用 dt 累加器
   *    而不是「每帧跑一次」。
   *
   *  顺带一个好处：FFT 从「每帧一次」降到「每秒 30 次」，
   *  高帧率下 CPU 反而更低了。 */
  const double specDt = 1.0 / 30.0;
  specAccum_ += dt;
  int guard = 0;
  while (specAccum_ >= specDt && guard++ < 4) {
    specAccum_ -= specDt;
    computeSpectrum128();
  }
  if (specAccum_ > specDt * 4.0) specAccum_ = 0.0;   // 卡顿后别追帧追爆

  /* 分析层：按 anaHz 跑（默认 250，= 桌面版实测的那个定时器频率）。
     渲染 60fps 时每帧约 4.2 次 —— 和桌面版一样：渲染一帧，
     分析已经在后台跑过好几轮了，画面读到的永远是「最近一次」的结果。
     ⚠ guard 要给够：渲染掉到 30fps 时每帧要补 ~8 次；
       卡顿后最多补 64 次，再多就说明是长时间挂起，直接放弃追赶。 */
  const double anaDt = 1.0 / (anaHz > 1.0 ? anaHz : 1.0);
  anaAccum_ += dt;
  guard = 0;
  while (anaAccum_ >= anaDt && guard++ < 64) {
    anaAccum_ -= anaDt;
    analyze512();
  }
  if (anaAccum_ > anaDt * 64.0) anaAccum_ = 0.0;

  /* 空闲波浪：连续安静 debounce 秒后淡入到 1，有声音立刻淡出到 0。
     ⚠ 过渡是按**时间**积分的（1/fade * dt），不是按帧 —— 否则高刷屏上会快好几倍。 */
  double target;
  if (audioReceived_) {
    if (lastActiveTime_ > lastIdleTime_) {
      target = 0.0;
      idleStartTime_ = 0.0;
    } else {
      if (idleStartTime_ == 0.0) idleStartTime_ = nowMs_;
      target = ((nowMs_ - idleStartTime_) / 1000.0 >= idleDebounce) ? 1.0 : 0.0;
    }
  } else {
    target = 1.0;
  }
  const double step = (1.0 / std::max(0.01, idleFade)) * dt;
  if (target > curIdle_) curIdle_ = std::min(target, curIdle_ + step);
  else if (target < curIdle_) curIdle_ = std::max(target, curIdle_ - step);
  idleWave = idleEnabled ? (float)curIdle_ : 0.0f;
}
