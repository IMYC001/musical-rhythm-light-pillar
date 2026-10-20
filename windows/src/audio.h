/*
 * 音频：WASAPI 环回捕获 → FFT → 作品自己的分析链。
 *
 * 链路完全照搬作品 + browser-bridge 的现有实现，分四段：
 *
 *   ① WASAPI loopback 抓系统声音（立体声 float32，48kHz 为主）
 *   ② 2048 点 FFT（Blackman 窗）→ 每声道 64 段电平 0~1   ← 等价 Web Audio AnalyserNode
 *   ③ 128 值（64 左 + 64 右）→ 4 倍上采样成 512 桶        ← 等价 setWallpaperAudioData
 *   ④ 512 桶 → 10 个频段包络 + 平滑                      ← 等价作品 bundle 里的 getAudioData
 *
 * ⚠ ②③④ 三段的常数一个都不能改。作品是在 WE 的 128 值格式下调的参，
 *   bridge 的作者又是照着 WE 的物理含义反推的 64 段划分（0–16kHz 等宽）。
 *   任何一处「等价改写」都会让 drive 的摆幅对不上，表现是光柱要么不动、要么顶满。
 */
#pragma once

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

/* ---- ② 的参数（对应 bridge 里的同名常量）---- */
static const int LP_CH_BINS = 64;      // 每声道 64 段
static const int LP_OUT_BINS = 128;    // 64 左 + 64 右
static const int LP_FFT_SIZE = 2048;
static const double LP_MAX_HZ = 16000.0;
/* 等比（log）频率轴的下限，对应 bridge 的 `BAND_LO_HZ = 30`。
   等比模式下第 i 段下边界 = 30 * (16000/30)^(i/64)。 */
static const double LP_BAND_LO_HZ = 30.0;
static const double LP_DB_MIN = -100.0;
static const double LP_DB_MAX = -20.0;
static const double LP_LV_MIN0 = -53.5;
static const double LP_SPECTRAL_TILT = 5.5;
static const double LP_TILT_REF_HZ = 125.0;

/* ---- 段电平自适应（perband）---- */
static const int LP_RP_WIN = 300;
static const int LP_RP_BINS = 64;
static const double LP_RP_DBMIN = -110.0;
static const double LP_RP_DBMAX = -10.0;

struct LpBands {
  float subBass = 0, bass = 0, lowMid = 0, mid = 0, highMid = 0;
  float energy = 0, presence = 0, brilliance = 0, air = 0, brightness = 0;
};

class LpAudio {
 public:
  LpAudio();
  ~LpAudio();

  /* ---- 可调参数（面板上能改的都在这儿）---- */
  double gainDb = 0.0;
  double widthDb = 46.0;
  double balance = 0.85;
  int mapMode = 0;    // 0 = perband（每段自适应）1 = window（全频段共用窗口）
  int bandMode = 0;   // 0 = linear（0–16kHz 等宽）1 = log
  double level = 0.15;
  double contrast = 1.8;
  double loPct = 0.20;
  double hiPct = 0.99;
  double minSpan = 4.0;
  /* 滚动直方图的窗口长度（界面上的「跟踪窗口」，单位秒）。
     ⚠ 直方图挂在**频谱**那一层（固定 30Hz，= 原作品的 `PUSH_HZ`），
       所以窗口长度永远是「秒 × 30」帧 = 默认 300 帧 / 10 秒，
       和渲染帧率无关 —— 改帧率不需要重分配、也不需要重新攒满 10 秒。 */
  double rpWinSec = 10.0;

  /* 作品自己的两个属性（project.json 的默认值）*/
  double intensity = 1.0;       // uAudioIntensity
  double responseRange = 1.0;   // uResponseRange

  /* ---- 分析层频率（Hz）----
     原作品把音频分析（getAudioData，含那个一阶平滑 ee）放在一个
     `setInterval(ht, 1e3/1e3)` 里 —— 请求 1ms，Chromium 把嵌套定时器
     钳到 4ms，所以**实际约 250 次/秒**。
     ⚠ 这个数不能拍脑袋：ee = 0.15 是「每次调用作用一次」、不按 dt 缩放的，
       所以频率直接决定包络时间常数 τ = -1/(f·ln0.85)：
         250Hz → 24.6ms（桌面版实测值）
          60Hz → 111ms   （慢 4.5 倍：幅度被压小、瞬态被抹平）
       实测方法与证据见 tools/README-对照台.md。
     默认 250 = 桌面版实测值；改它等于改原作品那个定时器的周期。 */
  double anaHz = 250.0;

  /* 空闲波浪 */
  bool idleEnabled = true;
  double idleDebounce = 1.0;
  double idleFade = 1.0;

  bool start();          // 起捕获线程；失败返回 false
  void stop();
  bool running() const { return running_.load(); }
  const char* lastError() const { return err_; }

  /* 渲染循环每帧调一次：推进分析、更新 bands / idleWave */
  void tick(double dt);

  LpBands bands;         // 已平滑的频段包络（直接喂 uniform）
  float idleWave = 0.0;  // uIdleWave
  float meterLevel = 0.0;  // 0~1，给面板的电平表

  /* 把可调参数改完后的副作用集中做掉（重建频段表 / 重置跟踪器）*/
  void paramsChanged();
  /* 改了 anaHz 之后调一下，把分析累加器清零 ——
     否则会拿旧周期算出来的余量去按新周期补跑一批分析，
     表现是「改分析频率的那一瞬间包络突跳一下」。
     ⚠ 只清累加器，**不动**滚动直方图（直方图挂在 30Hz 的频谱层上，
       与分析频率无关，清空它会让分位归一化重新攒 10 秒）。 */
  void resetAnalysisClock() { anaAccum_ = 0.0; }
  /* 只重算 dB 窗口与倾斜表，**不动**滚动直方图。
     拖「增益 / 对比度 / 总电平」这类纯数值滑杆时用这个 —— 见 main.cpp 里
     pushAudioParamsLight() 的注释（每次都清空直方图会让面板看着像坏的）。 */
  void rebuildTiltOnly();
  /* 重置「每段自适应」的滚动直方图与倾斜表。改了频段划分 / 分位定义 /
     映射方式之后必须调（旧的统计口径已经作废）。 */
  void resetTrackers();
  void rebuildTilt();

  /* ---- 测试钩子（只给 audiotest.exe 用，正常运行不碰）----
     直接喂 128 个频谱值（等价 bridge 的 spectrum 数组）并跑一次分析，
     不经过 FFT、不经过音频线程。
     为什么要这个东西：用户报「和 Electron 版观感差不少、幅度偏小、部分节拍
     不响应」—— 这类问题靠肉眼和猜都定位不到，必须把**原生分析链**和
     **原作品 JS 的分析链**喂同一串输入、逐帧比输出。
     录音路径：光柱.exe --dumpspec 录下真实音乐推出来的 128 个频谱值，
     然后 audiotest.exe 和 tools/jsref.js 各跑一遍，diff 结果。 */
  void testFeedSpectrum(const double* spec128);
  const double* spectrum() const { return spectrum_; }
  /* 16 个平滑量的原始数组（顺序 = 原作品 smoothedData 的键顺序）：
       [0]bass [1]mid [2]treble [3]energy [4]subBass [5]lowMid [6]highMid
       [7]presence [8]brilliance [9]air [10]warmth [11]brightness
       [12]sharpness [13]smoothness [14]density [15]spectralCentroid
     ⚠ 顺序必须和 tools/orig-analyzer.js 的导出顺序、以及 audiotest.cpp 的
       表头三者严格一致 —— 这是逐帧比对的前提。 */
  const double* smoothed() const { return smooth_; }

  /* ---- --dumpspec：把每一帧的 128 个频谱值落盘（只给测试用）----
     直接挂在 computeSpectrum128() 的末尾，所以**一帧都不会漏**。
     不在渲染循环里轮询是有原因的：频谱 30Hz、渲染 60Hz 时两帧一读正好，
     但渲染掉到 30fps 以下（或者卡一下）就会漏掉整帧，
     而漏帧对「逐帧比对」是致命的 —— 后面每一帧都对不齐。 */
  bool startDump(const wchar_t* path, const char* label);
  void stopDump();

  /* `--bands` 诊断：把两种频率轴各自的频段划分打出来，用来一眼确认
     「切等宽/等比到底改了哪些东西」。
     ⚠ 内部会临时切 bandMode 再还原，所以只该在**还没开音频**时调用
       （开了之后切 bandMode 需要重建表，正是这个诊断要解释的那件事）。 */
  void dumpBands(FILE* f, double sampleRate);

  /* 频段划分表的指纹（只取决于 bandK0_/bandK1_，与音频数据无关）。
     自检用它来证明「频率轴」这一项真的生效了 —— 这一项曾经切了完全没反应：
     控件值会变（`*it.ival` 当然会变），但频段划分表只在 start() 里建过一次，
     所以「控件值变了」**不能**当判据，必须看表的指纹。 */
  long long bandSignature() const;

 private:
  void captureLoop();
  void buildBands();
  /* 第 i 段的下边界频率（Hz）。linear = 0–16k 等宽，log = 30–16k 等比。
     ★ buildBands() 和 rebuildTilt() **必须都走这一个函数** ——
       原作品里 rebuildTilt 的参考频率也是从 bandEdge(i) 取的，
       所以切等比时倾斜补偿量要跟着变（见 audio.cpp 里的注释）。 */
  double bandEdgeHz(double i) const;
  void computeSpectrum128();          // ② → spectrum_[0..127]
  void analyze512();                  // ③④ → bands

  /* WASAPI */
  std::atomic<bool> running_{false};
  std::atomic<bool> quit_{false};
  std::thread th_;
  const char* err_ = nullptr;
  double sampleRate_ = 48000.0;
  int channels_ = 2;

  /* 捕获到的交错样本（float32）。渲染循环每次取走全部，
     所以它天然只保留「上一帧到现在」的那一小段。 */
  std::mutex bufMu_;
  std::vector<float> pending_;   // 交错 LRLRLR…

  /* ② 的中间量 */
  int bandK0_[LP_CH_BINS], bandK1_[LP_CH_BINS];
  double bandTilt_[LP_CH_BINS];
  double fftRe_[LP_FFT_SIZE], fftIm_[LP_FFT_SIZE];
  double win_[LP_FFT_SIZE];
  double spectrum_[LP_OUT_BINS];   // 0~1
  double rawDb_[LP_OUT_BINS];
  double lvMin_ = LP_LV_MIN0, lvMax_ = LP_LV_MIN0 + 46.0;

  /* 滚动直方图 */
  std::vector<int16_t> rpRing_;
  std::vector<uint16_t> rpHist_;
  int rpWin_ = LP_RP_WIN;   // 实际窗口帧数（由 rpWinSec × 30Hz 换算）
  int rpPos_ = 0, rpFrame_ = 0;

  /* ③④ 的中间量 */
  double bucket_[512];
  double prevBucket_[512];
  double smooth_[16] = {0};   // bass mid treble energy subBass lowMid highMid presence brilliance air warmth brightness sharpness smoothness density spectralCentroid
  double prevBrightness_ = 0;
  double lastActiveTime_ = 0, lastIdleTime_ = 0, idleStartTime_ = 0;
  double curIdle_ = 0;
  double nowMs_ = 0;
  double specAccum_ = 0.0;   // 频谱（30Hz）的累加器
  double anaAccum_ = 0.0;    // 分析（anaHz）的累加器
  bool audioReceived_ = false;

  /* --dumpspec 的落盘句柄（正常运行时是 nullptr，一次都不进） */
  FILE* dump_ = nullptr;
  std::string dumpLabel_;
  long dumpFrames_ = 0;
};
