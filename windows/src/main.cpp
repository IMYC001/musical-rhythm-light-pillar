/*
 * 光柱 · 原生桌面覆盖层（Windows / D3D11）
 *
 * 一份网页版光柱可视化的原生移植。
 * 视觉与音频分析链沿用参考实现，只把「画在网页上」换成「叠在桌面上」。
 *
 * 组成：
 *   · 覆盖层窗口  —— 每块显示器一个，全屏透明、常驻最上层、鼠标穿透
 *   · 托盘图标    —— 左键开面板，右键出菜单（面板 / 显示在哪块屏 / 退出）
 *   · 调节面板    —— 原生控件窗口，改动即时生效
 *   · 音频        —— WASAPI 环回 → FFT → 参考实现里的分析器（见 audio.cpp）
 *
 * 所有「为什么这么写」的关键注释都在对应代码旁边，尤其是踩过的坑。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "audio.h"
#include "d3d.h"
#include "panel.h"
#include "prompt.h"
#include "data_themes.h"
#include "shader_src.h"

/* ============================ 常量与全局 ============================ */

static const wchar_t* LP_APP_NAME = L"光柱";
static const wchar_t* LP_WND_CLASS_TRAY = L"LpTrayWindow";
static const wchar_t* LP_MUTEX_NAME = L"Local\\LightPillarNative_SingleInstance";

#define WM_LP_TRAY (WM_APP + 1)
#define ID_TRAY_ICON 1
#define HOTKEY_QUIT 1

enum {
  ID_QUIT = 40001,
  ID_OPEN_PANEL,
  ID_DISPLAY_ALL,
  ID_DISPLAY_BASE = 40100,  // + 显示器序号
  ID_RESET = 40200,
  ID_TRACK_BASE = 41000,    // + 参数序号
  ID_COMBO_BASE = 42000,
  ID_CHECK_BASE = 43000,
  ID_VALUE_BASE = 44000,    // 数值标签
  ID_METER = 45000,
  ID_STATUS = 45001,
  ID_DISPLAY_COMBO = 45002,
  ID_FPS_COMBO = 45003,
};

/* ============================ 设置 ============================ */

struct LpSettings {
  /* 外观 */
  int styleMode = 0;            // 0 = 光柱跳动，1 = 常驻光柱 · 节拍闪色
  float beamInset = 0.0f;       // -0.15 .. 0.30
  int themeIndex = 0;           // LP_THEMES 下标
  bool themeAuto = false;       // 自动轮询
  float themeCycleSec = 60.0f;  // 10 .. 300
  /* 音频响应（参考实现里的两个属性，默认值与上游工程配置一致） */
  float audioIntensity = 1.0f;  // 0.3 .. 2.5
  float responseRange = 1.0f;   // 0.3 .. 2.0
  /* 强调色 */
  bool peakEnabled = true;
  float peakIntensity = 1.0f;   // 0 .. 2
  /* 空闲波浪 */
  bool idleEnabled = true;
  float idleDebounce = 1.0f;    // 0.5 .. 5
  float idleFade = 1.0f;        // 0.5 .. 5
  /* 桌面版专属：光柱实心度 */
  float alphaK = 0.10f;         // 0 .. 1
  /* 性能 */
  int fpsLimit = 60;            // 30 / 60 / 120 / 144   —— 渲染帧率（= 参考实现 general.fps）
  /* 分析频率：参考实现把音频分析放在 `setInterval(ht, 1e3/1e3)` 里，
     请求 1ms、Chromium 钳到 4ms → **实测 249.6 次/秒**。
     它里面的平滑系数 ee=0.15 不按 dt 缩放，所以这个频率**就是**包络的时间常数：
       250Hz → 24.6ms；60Hz → 111ms（慢 4.5 倍，幅度被压小、弱拍被抹平）。
     ⚠ 别把它跟 fpsLimit 混在一起 —— 桌面版里那是两条独立的时钟。 */
  int anaHz = 250;              // 30 / 60 / 120 / 250
  /* 显示在哪块屏（-1 = 全部） */
  int displayIndex = -1;
  /* 音频管线校准（对应 桥接层 声音页那几个滑杆）。
     ⚠ 范围一律照抄浏览器版（桥接层 的 AUDIO_PARAMS + addTune），
       注释里写的就是界面上的 min..max。第一版好几项都写窄了，
       最要命的是 level 上限写成 0.5（浏览器是 1.0）。 */
  float gainDb = 0.0f;          // -12 .. 12
  float widthDb = 46.0f;        // 15 .. 60
  float balance = 0.85f;        // 0 .. 1
  int mapMode = 0;              // 0 = perband（每段自适应），1 = window（共用窗口）
  int bandMode = 0;             // 0 = linear，1 = log
  float contrast = 1.8f;        // 1 .. 3
  float level = 0.15f;          // 0.05 .. 1        ← 浏览器上限是 1.0
  float loPct = 0.20f;          // 0.02 .. 0.5      （常态分位）
  float hiPct = 0.99f;          // 0.8 .. 1         （峰值分位）
  float rpWinSec = 10.0f;       // 3 .. 30          （跟踪窗口，秒）
  /* 声音来源：0 = 系统声音环回，1 = 空闲（不接输入） */
  int audioIdleMode = 0;
};

static LpSettings g_set;
static LpAudio g_audio;
static HINSTANCE g_hInst = nullptr;
static std::vector<LpD3DWindow*> g_overlays;
static std::vector<LpMonitor> g_mons;
static HWND g_trayHwnd = nullptr;
static HWND g_panel = nullptr;
static bool g_quitting = false;
static bool g_audioOk = false;
static bool g_trayOk = false;
/* 自检模式下打开交换链回读。放在这里是因为覆盖层会被重建（换显示器 / 恢复默认），
   重建出来的窗口也得带上这个标志，否则回读会在中途悄悄失效。 */
static bool g_selfTestMode = false;
/* 纯诊断：跳过 Present（见 d3d.h）。必须放全局 —— 覆盖层会因为
   换显示器 / 恢复默认 / 设备丢失被重建，重建出来的窗口也得带上这个标志。 */
static bool g_noPresent = false;
/* 纯诊断：把覆盖层窗口四边各内缩 N 像素。
   用来验证「每 ~4.86 秒一次、约 400ms 的 Present 阻塞」是不是 DWM 的
   全屏检测 / 硬件平面（MPO）重配置引起的 —— 只要窗口不是正好铺满整屏，
   全屏检测就不会触发。正常运行恒为 0。 */
static int g_winInset = 0;
/* 诊断：让 DwmFlush 接管限帧（自己就不用再等定时器了）。 */
static bool g_dwmFlushPace = false;
/* 诊断：跳过 timeBeginPeriod(1)。
   timeBeginPeriod 会把系统计时器精度拉到 1ms，代价是全局滴答变密 ——
   已知会和某些显卡驱动 / DWM 的帧调度互相干扰。纯计时对照探针没调它，
   所以这也是一个需要排除的变量。 */
static bool g_no1ms = false;
/* 诊断：只把「读配置 → 读预设」这一步做完就打印退出，不建覆盖层。
   用来回答「我的设置去哪了」—— 面板显示的是内存里的值，
   光看面板看不出启动时被哪一步改过。 */
static bool g_showCfg = false;
static std::string g_audioErr;
static double g_time = 0.0;          // 累计秒数，喂 uTime
static float g_themeClock = 0.0f;    // 自动轮询计时

/* 参数表（面板与设置之间的唯一映射）
 *
 *  ⚠ 这些范围**必须**和参考实现浏览器版面板一致（桥接层 里的
 *    PROPS / AUDIO_PARAMS）。第一版移植时「总电平」上限写成了 0.5，
 *    而浏览器版是 1.0 —— 用户把滑杆推到底也只有一半电平，
 *    表现就是「不管怎么调参数跳动幅度都很小」。范围对不上的滑杆
 *    比没有这个滑杆更糟：用户会以为功能坏了。 */
enum SId {
  S_STYLE = 0, S_THEME, S_THEME_CYCLE, S_BEAM_INSET, S_ALPHA_K,
  S_AUDIO_INT, S_RESP_RANGE, S_PEAK_EN, S_PEAK_INT,
  S_IDLE_EN, S_IDLE_DEBOUNCE, S_IDLE_FADE,
  S_FPS, S_ANA_HZ, S_DISPLAY, S_SOURCE,
  S_GAIN_DB, S_WIDTH_DB, S_BALANCE, S_MAP_MODE, S_BAND_MODE,
  S_CONTRAST, S_LEVEL, S_LO_PCT, S_HI_PCT, S_WIN_SEC,
  S_RESET,
  /* 预设区（对应 桥接层 里参数页最顶上那块） */
  S_PRESET_SEL, S_PRESET_SAVE, S_PRESET_RENAME, S_PRESET_DEL,
  S_COUNT
};

/* 面板上有几个「宿主侧代理量」：面板直接绑 int*，但底层状态不是单个 int。
   （主题下拉的 0 是「自动轮询」、显示器下拉的 0 是「全部」、帧率下拉是索引。）
   这些代理量在 syncPanelControls() 里与真实状态对齐。 */
static int g_themeSel = 0;     // 0 = 自动轮询，1..N = themeIndex + 1
static int g_displaySel = 0;   // 0 = 全部，1..N = displayIndex + 1
static int g_fpsSel = 1;       // 0/1/2/3 → 30/60/120/144
static int g_anaSel = 3;       // 0/1/2/3 → 30/60/120/250（分析频率）
static int g_sourceSel = 0;    // 0 = 系统声音环回，1 = 空闲
/* 预设下拉的值：0 = 未选择，1..N = g_presets[k-1]，N+1 = 「＋ 新建预设…」 */
static int g_presetSel = 0;
/* 面板里那个下拉的项下标（-1 = 面板还没建）。预设增删时原地改它的 options。
   放在这里而不是预设那段代码旁边：buildPanelItems() 要用它。 */
static int g_presetSelItem = -1;

/* ⚠ 这里原来有个 `fpsToPushHz()`（把渲染帧率当成「分析推送频率」）——
 *   **v6 已删除**，因为它背后的模型是错的。
 *
 *  参考实现里其实有**两层不同的频率**（见 audio.cpp 的 tick()）：
 *
 *    ① 数据层：FFT → 每声道 64 段 → 128 个频谱值，固定 **30Hz**
 *       （`桥接层` 的 `PUSH_HZ = 30`）。滚动直方图挂在这层。
 *    ② 分析层：128 → 512 桶 → 8 频段 → 16 个量的一阶平滑，
 *       跟着**渲染帧率**跑（`getAudioData()` 在 rAF 里，频率由 `FPS_LIMIT` 定）。
 *
 *  v5 把两层都挂在渲染帧率上、再给平滑系数做「30Hz 时间常数换算」，
 *  结果时间常数变成参考实现的 1.93 倍、**包络峰值只剩 67%** ——
 *  正是用户报的「响应幅度没有 Electron 大、部分节拍不响应」。
 *  v6 改成：数据层钉死 30Hz、分析层每帧跑、平滑系数用原始值不换算。
 *  于是 fpsLimit 只影响**渲染与分析**频率，不再需要任何换算，
 *  也就没有 `fpsToPushHz` 存在的理由了。 */

static void pushAudioParams();   // 前向声明

/* 音频管线参数：分成「贵的」和「便宜的」两组。
 *
 *  ⚠ 这是第一版的第二个真 bug：原来无论改哪个音频参数都调 paramsChanged()，
 *    而它会 resetTrackers() —— 把 10 秒的滚动直方图整个清空。
 *    于是「每段自适应」的分位归一化要从零重新攒 10 秒，期间光柱忽大忽小、
 *    推到哪都不对。用户拖一下滑杆就重置一次，当然「怎么调都没效果」。
 *    只有真正改变了频段划分 / 分位定义 / 映射方式的参数才需要重置跟踪器。 */
static void pushAudioParams() {
  g_audio.gainDb = g_set.gainDb;
  g_audio.widthDb = g_set.widthDb;
  g_audio.balance = g_set.balance;
  g_audio.mapMode = g_set.mapMode;
  g_audio.bandMode = g_set.bandMode;
  g_audio.contrast = g_set.contrast;
  g_audio.level = g_set.level;
  g_audio.loPct = g_set.loPct;
  g_audio.hiPct = g_set.hiPct;
  g_audio.rpWinSec = g_set.rpWinSec;
  g_audio.intensity = g_set.audioIntensity;
  g_audio.responseRange = g_set.responseRange;
  g_audio.idleEnabled = g_set.idleEnabled;
  g_audio.idleDebounce = g_set.idleDebounce;
  g_audio.idleFade = g_set.idleFade;
  /* ★ v6：这里**不再**设置 pushHz。频谱那一层固定 30Hz（参考实现的
     `PUSH_HZ`），分析层按 anaHz 跑（默认 250 = 桌面版实测的定时器频率）——
     两者都与渲染帧率无关，见 tick() 的三层频率注释。 */
  g_audio.anaHz = (double)g_set.anaHz;
  /* ★★★ 必须走 paramsChanged()，不能只 rebuildTiltOnly()。
     参考实现的桥接层里 `bandMode` 的 setter 是：
         set: function (v) { BAND_MODE = ...; resetTrackers(); if (audioCtx) buildBands(); }
     —— 最后那句 `buildBands()` 我们**漏掉了**，后果就是「频率轴」这一项
     在面板上怎么切都没反应：
       · bandMode 只是被存进 g_set / g_audio，谁也不会去重建 bandK0_/bandK1_；
       · 唯一的 buildBands() 调用在 g_audio.start() 里（只在启动时跑一次），
         所以 ini 里写死的值在启动时是生效的 —— 面板上改就不生效，
         而且「重启后好了」这个现象很难让人联想到是这条链路。
       · paramsChanged() 本来就写着 buildBands() + resetTrackers()，
         但它**从来没有被调用过**（死代码）—— 这个函数存在的唯一意义
         就是这里该调它。
     pushAudioParams() 本来就已经 resetTrackers()（重档），
     所以换成 paramsChanged() 不会多付任何代价。 */
  g_audio.paramsChanged();
}

/* 只改了「数值大小」的参数（增益 / 对比度 / 总电平 / 窗口宽度 / 频谱平衡）：
   重算 dB 窗口和倾斜表就够，**不要**清空滚动直方图。 */
static void pushAudioParamsLight() {
  g_audio.gainDb = g_set.gainDb;
  g_audio.widthDb = g_set.widthDb;
  g_audio.balance = g_set.balance;
  g_audio.contrast = g_set.contrast;
  g_audio.level = g_set.level;
  g_audio.rebuildTiltOnly();
}

static bool isLightAudioParam(int id) {
  return id == S_GAIN_DB || id == S_WIDTH_DB || id == S_BALANCE ||
         id == S_CONTRAST || id == S_LEVEL;
}

/* ---- INI 持久化：放 %APPDATA%\LightPillar\settings.ini ---- */

/* 配置版本号。改了参数**范围**或**默认值**就要 +1 —— 老配置里按旧范围
   拖出来的值在新范围下是错的，必须在载入时回退（见 loadSettings 里的迁移）。 */
static const int kCfgVer = 2;
/* 本次启动是否发生过配置迁移（迁了就要立刻回写一次，否则每次启动都重迁） */
static bool g_migrated = false;

static std::wstring dataDir() {
  wchar_t dir[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir)))
    return L".";
  std::wstring p = std::wstring(dir) + L"\\LightPillar";
  CreateDirectoryW(p.c_str(), nullptr);
  return p;
}

/* 自检会把两个路径都改到输出目录去 —— 自检绝不许动用户自己的配置文件。
   ⚠ iniPath 也必须有这个覆盖：自检要验「存盘 → 读回 → 载预设」整条启动顺序，
     没有覆盖的话它会去写用户真实的 settings.ini。 */
static std::wstring g_iniPathOverride;
static std::wstring g_presetPathOverride;
static std::wstring iniPath() {
  if (!g_iniPathOverride.empty()) return g_iniPathOverride;
  return dataDir() + L"\\settings.ini";
}
/* 参数预设另存一个文件：它和「当前配置」生命周期不同 ——
   配置只有一个、随时被覆盖，预设是多份、由用户显式增删。 */
static std::wstring presetPath() {
  if (!g_presetPathOverride.empty()) return g_presetPathOverride;
  return dataDir() + L"\\presets.ini";
}

/* ================== 配置读写：唯一的一份参数清单 ==================
 *
 * 写 ini / 读 ini / 存预设 / 载预设 **共用同一份清单**（visitAllSettings），
 * 方向由 io.saving 决定：
 *     saving = true   → 把字段的值写进 io
 *     saving = false  → 从 io 取值（取不到就用默认值）写回字段
 *
 * 为什么非要这样：以前 saveSettings 和 loadSettings 各写一遍 27 个键，
 * 于是真出过事 —— loPct / hiPct / rpWinSec / audioIdleMode 四项「只写不读」，
 * 用户改完重启全被打回默认值，表现就是「改了没用」，而且完全看不出来
 * （面板显示的是内存里的新值）。
 * 现在加一个参数只在 visitAllSettings 里加一行，四个入口自动全覆盖；
 * 预设也不可能漏掉某个参数。
 */
struct LpCfgIo {
  bool saving = false;
  virtual ~LpCfgIo() {}
  virtual void vInt(const wchar_t* k, int* v, int def) = 0;
  virtual void vFloat(const wchar_t* k, float* v, float def) = 0;
  virtual void vBool(const wchar_t* k, bool* v, bool def) = 0;
};

/* ★★★ 唯一的参数清单。新增参数只改这里。 */
static void visitAllSettings(LpCfgIo& io) {
  io.vInt(L"styleMode", &g_set.styleMode, 0);
  io.vFloat(L"beamInset", &g_set.beamInset, 0.0f);
  io.vInt(L"themeIndex", &g_set.themeIndex, 0);
  io.vBool(L"themeAuto", &g_set.themeAuto, false);
  io.vFloat(L"themeCycleSec", &g_set.themeCycleSec, 60.0f);
  io.vFloat(L"audioIntensity", &g_set.audioIntensity, 1.0f);
  io.vFloat(L"responseRange", &g_set.responseRange, 1.0f);
  io.vBool(L"peakEnabled", &g_set.peakEnabled, true);
  io.vFloat(L"peakIntensity", &g_set.peakIntensity, 1.0f);
  io.vBool(L"idleEnabled", &g_set.idleEnabled, true);
  io.vFloat(L"idleDebounce", &g_set.idleDebounce, 1.0f);
  io.vFloat(L"idleFade", &g_set.idleFade, 1.0f);
  io.vFloat(L"alphaK", &g_set.alphaK, 0.10f);
  io.vInt(L"fpsLimit", &g_set.fpsLimit, 60);
  /* anaHz 是 v6 新增的：老配置里没有这个键 → 取 250（桌面版实测值）。
     ⚠ 默认值必须是 250，不能是 60 —— 60 会让包络慢 4.5 倍，
       正是用户报的「幅度没有桌面版大、部分节拍不响应」。 */
  io.vInt(L"anaHz", &g_set.anaHz, 250);
  io.vInt(L"displayIndex", &g_set.displayIndex, -1);
  io.vFloat(L"gainDb", &g_set.gainDb, 0.0f);
  io.vFloat(L"widthDb", &g_set.widthDb, 46.0f);
  io.vFloat(L"balance", &g_set.balance, 0.85f);
  io.vInt(L"mapMode", &g_set.mapMode, 0);
  io.vInt(L"bandMode", &g_set.bandMode, 0);
  io.vFloat(L"contrast", &g_set.contrast, 1.8f);
  io.vFloat(L"level", &g_set.level, 0.15f);
  io.vFloat(L"loPct", &g_set.loPct, 0.20f);
  io.vFloat(L"hiPct", &g_set.hiPct, 0.99f);
  io.vFloat(L"rpWinSec", &g_set.rpWinSec, 10.0f);
  io.vInt(L"audioIdleMode", &g_set.audioIdleMode, 0);
}

/* ---- ini 版 ---- */
struct IniIo : LpCfgIo {
  std::wstring path;
  explicit IniIo(std::wstring p) : path(std::move(p)) {}
  void vInt(const wchar_t* k, int* v, int def) override {
    if (saving) {
      wchar_t b[32] = {};
      swprintf(b, 32, L"%d", *v);
      WritePrivateProfileStringW(L"app", k, b, path.c_str());
    } else {
      *v = (int)GetPrivateProfileIntW(L"app", k, def, path.c_str());
    }
  }
  void vFloat(const wchar_t* k, float* v, float def) override {
    if (saving) {
      wchar_t b[40] = {};
      swprintf(b, 40, L"%.6g", *v);
      WritePrivateProfileStringW(L"app", k, b, path.c_str());
    } else {
      wchar_t b[64] = {};
      GetPrivateProfileStringW(L"app", k, L"", b, 63, path.c_str());
      *v = b[0] ? (float)_wtof(b) : def;
    }
  }
  void vBool(const wchar_t* k, bool* v, bool def) override {
    if (saving) WritePrivateProfileStringW(L"app", k, *v ? L"1" : L"0", path.c_str());
    else *v = GetPrivateProfileIntW(L"app", k, def ? 1 : 0, path.c_str()) != 0;
  }
};

/* ---- 预设版：值放在内存里的一张**有序**键值表 ----
   有序是为了让写出来的 presets.ini 键序和 visitAllSettings 一致，人看着舒服。 */
typedef std::vector<std::pair<std::wstring, std::wstring>> LpKvList;

struct PresetIo : LpCfgIo {
  LpKvList* kv = nullptr;
  explicit PresetIo(LpKvList* k) : kv(k) {}
  const std::wstring* find(const wchar_t* k) const {
    for (const auto& p : *kv) {
      if (p.first == k) return &p.second;
    }
    return nullptr;
  }
  void put(const wchar_t* k, const std::wstring& s) { kv->push_back(std::make_pair(k, s)); }
  void vInt(const wchar_t* k, int* v, int def) override {
    if (saving) {
      wchar_t b[32] = {};
      swprintf(b, 32, L"%d", *v);
      put(k, b);
    } else {
      const std::wstring* s = find(k);
      *v = s ? (int)wcstol(s->c_str(), nullptr, 10) : def;
    }
  }
  void vFloat(const wchar_t* k, float* v, float def) override {
    if (saving) {
      wchar_t b[40] = {};
      swprintf(b, 40, L"%.6g", *v);
      put(k, b);
    } else {
      const std::wstring* s = find(k);
      *v = s ? (float)_wtof(s->c_str()) : def;
    }
  }
  void vBool(const wchar_t* k, bool* v, bool def) override {
    if (saving) put(k, *v ? L"1" : L"0");
    else {
      const std::wstring* s = find(k);
      *v = s ? (wcstol(s->c_str(), nullptr, 10) != 0) : def;
    }
  }
};

/* ---- 磁盘版：ini 文件 → kv ----
 *
 * ★★★ 这个类是为一次真实事故补上的，别删。
 *
 * 起因：loadPresets() 原来写的是
 *     PresetIo io(&ps.kv); io.saving = false; visitAllSettings(io);
 * 但 PresetIo 的「读」方向是 **kv → g_set**，不是 ini → kv。于是：
 *     ps.kv 一直是空的 → visitAllSettings 把每一项都当成「文件里没有」→
 *     **取默认值写回 g_set**。两个后果同时发生，而且都极难看出：
 *   ① loadPresets() 在启动时紧跟 loadSettings() 跑 → **每次启动都把用户的设置
 *      冲成出厂默认值**，然后在退出 / 面板一改动时把这份默认值写回 settings.ini
 *      （用户看到的就是「改完重启全没了」）；
 *   ② 预设的值压根没从文件读进来 → 选中任何预设都只会「恢复默认」。
 *
 * 教训：**「读」和「写」是两个不同的方向。** 共用一张 kv 的类只能覆盖
 * kv ⇄ g_set 这两个方向；ini ⇄ kv 必须是第三个方向，不能复用。
 *
 * ⚠ 而且自检当时查不出这个 bug —— 因为自检是从「出厂默认值」出发的，
 *   把默认值冲成默认值，看不出任何差别。所以自检里必须有一步
 *   「先把 g_set 设成非默认值，再调 loadPresets()，看它有没有被动过」。
 */
struct PresetLoadIo : LpCfgIo {
  LpKvList* kv = nullptr;
  std::wstring section, path;
  PresetLoadIo(LpKvList* k, std::wstring sec, std::wstring p)
      : kv(k), section(std::move(sec)), path(std::move(p)) {}
  /* 文件里没有这个键就落一个默认值进去 —— 让 kv 始终是「完整的一套」，
     以后新增参数时老预设读到的是新参数的默认值，而不是「缺失」。 */
  void one(const wchar_t* k, const wchar_t* def) {
    wchar_t buf[128] = {};
    GetPrivateProfileStringW(section.c_str(), k, def, buf, 127, path.c_str());
    kv->push_back(std::make_pair(std::wstring(k), std::wstring(buf)));
  }
  void vInt(const wchar_t* k, int* v, int def) override {
    (void)v;
    wchar_t d[32] = {};
    swprintf(d, 32, L"%d", def);
    one(k, d);
  }
  void vFloat(const wchar_t* k, float* v, float def) override {
    (void)v;
    wchar_t d[40] = {};
    swprintf(d, 40, L"%.6g", def);
    one(k, d);
  }
  void vBool(const wchar_t* k, bool* v, bool def) override {
    (void)v;
    one(k, def ? L"1" : L"0");
  }
};

/* ---- 范围钳制 ----
 * 无论配置从哪来（手改 ini、从更老的版本升上来、迁移、**载入预设**），
 * 越界值都不能进音频管线：level=5 会让画面直接爆掉，winSec=0 会让滚动直方图
 * 长度变成 0（然后除零）。宁可静默夹到边界，也不要带着越界值跑。
 * ⚠ 载入预设也走这里 —— 预设文件是用户能改的文本，同样不可信。 */
static void clampSettings() {
  auto cl = [](float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  };
  g_set.audioIntensity = cl(g_set.audioIntensity, 0.3f, 2.5f);
  g_set.responseRange = cl(g_set.responseRange, 0.3f, 2.0f);
  g_set.beamInset = cl(g_set.beamInset, -0.15f, 0.30f);
  g_set.themeCycleSec = cl(g_set.themeCycleSec, 10.0f, 300.0f);
  g_set.peakIntensity = cl(g_set.peakIntensity, 0.0f, 2.0f);
  g_set.idleDebounce = cl(g_set.idleDebounce, 0.5f, 5.0f);
  g_set.idleFade = cl(g_set.idleFade, 0.5f, 5.0f);
  g_set.alphaK = cl(g_set.alphaK, 0.0f, 1.0f);
  g_set.gainDb = cl(g_set.gainDb, -12.0f, 12.0f);
  g_set.widthDb = cl(g_set.widthDb, 15.0f, 60.0f);
  g_set.balance = cl(g_set.balance, 0.0f, 1.0f);
  g_set.contrast = cl(g_set.contrast, 1.0f, 3.0f);
  g_set.level = cl(g_set.level, 0.05f, 1.0f);
  g_set.loPct = cl(g_set.loPct, 0.02f, 0.50f);
  g_set.hiPct = cl(g_set.hiPct, 0.80f, 1.00f);
  g_set.rpWinSec = cl(g_set.rpWinSec, 3.0f, 30.0f);
  /* hi 必须严格大于 lo，否则分位区间退化成一个点，归一化除零 */
  if (g_set.hiPct <= g_set.loPct) g_set.hiPct = g_set.loPct + 0.10f;
  if (g_set.mapMode < 0 || g_set.mapMode > 1) g_set.mapMode = 0;
  if (g_set.bandMode < 0 || g_set.bandMode > 1) g_set.bandMode = 0;
  if (g_set.styleMode < 0 || g_set.styleMode > 1) g_set.styleMode = 0;
  if (g_set.audioIdleMode < 0 || g_set.audioIdleMode > 1) g_set.audioIdleMode = 0;
  if (g_set.themeIndex < 0 || g_set.themeIndex >= LP_THEME_COUNT) g_set.themeIndex = 0;
  if (g_set.fpsLimit != 30 && g_set.fpsLimit != 60 &&
      g_set.fpsLimit != 120 && g_set.fpsLimit != 144) g_set.fpsLimit = 60;
  /* 分析频率只认这四个值。⚠ 落回默认时是 250，不是 60 ——
     60 是「慢 4.5 倍」的那个错值，绝不能当兜底。 */
  if (g_set.anaHz != 30 && g_set.anaHz != 60 &&
      g_set.anaHz != 120 && g_set.anaHz != 250) g_set.anaHz = 250;
  if (g_set.displayIndex < -1) g_set.displayIndex = -1;
}

static void loadSettings() {
  IniIo io(iniPath());
  io.saving = false;
  visitAllSettings(io);

  /* ---- 配置迁移 ----
   * 第一版原生版的面板把好几项范围写窄了（level 上限 0.5、gainDb ±30、
   * widthDb 20..80、contrast 0.5..3），用户在那一版里拖出来的值直接搬进
   * 正确的范围就是「错的」：
   *   · level=0.5 已经是旧上限（浏览器默认 0.15、上限 1.0）；
   *   · mapMode=1（共用窗口）把 64 段塞进同一个 dB 窗口，每段的动态被压平
   *     —— 这正是用户说的「不管怎么调参数跳动幅度都很小」；
   *   · themeIndex=12 是调试时写进去的固定主题（浏览器默认是「霁紫」）。
   * 不清掉这些值，用户升到新版会觉得「参数还是调不动」。 */
  const int cfgVer = (int)GetPrivateProfileIntW(L"app", L"cfgVer", 0, iniPath().c_str());
  if (cfgVer < kCfgVer) {
    g_set.gainDb = 0.0f;        // 浏览器默认
    g_set.widthDb = 46.0f;
    g_set.balance = 0.85f;
    g_set.mapMode = 0;          // perband：每段自适应（浏览器默认）
    g_set.bandMode = 0;         // linear
    g_set.contrast = 1.8f;
    g_set.level = 0.15f;
    g_set.loPct = 0.20f;
    g_set.hiPct = 0.99f;
    g_set.rpWinSec = 10.0f;
    g_set.themeIndex = 0;       // 霁紫
    g_set.themeAuto = false;
    g_migrated = true;
  }

  clampSettings();
}

static void saveSettings() {
  /* 自检不许改用户的配置。判据是「**路径**有没有被改到别处」，而不是「是不是自检模式」——
     自检现在会把 iniPath 一起指到输出目录，指过去了就可以放心写
     （它要验「存盘 → 读回」这条路，不写就没得读）。
     按模式判断的话，自检永远测不到自己的读写往返。 */
  if (g_selfTestMode && g_iniPathOverride.empty()) return;
  const std::wstring p = iniPath();
  IniIo io(p);
  io.saving = true;
  visitAllSettings(io);
  /* 版本号必须**最后**写：它是「这份配置已经是新范围」的凭据，
     万一写一半崩了，版本号没落盘，下次启动会重新迁移一遍（安全方向）。 */
  wchar_t buf[32] = {};
  swprintf(buf, 32, L"%d", kCfgVer);
  WritePrivateProfileStringW(L"app", L"cfgVer", buf, p.c_str());
}

/* ============================ 覆盖层管理 ============================ */

static void destroyOverlays() {
  for (auto* w : g_overlays) {
    lp_d3d_destroy(w);
    delete w;
  }
  g_overlays.clear();
}

/* 按当前设置重建覆盖层。displayIndex = -1 表示每块显示器一个。 */
static bool buildOverlays(std::string* err) {
  destroyOverlays();
  lp_enum_monitors(&g_mons);

  std::vector<RECT> targets;
  if (g_set.displayIndex < 0 || g_set.displayIndex >= (int)g_mons.size()) {
    for (auto& m : g_mons) targets.push_back(m.rect);
  } else {
    targets.push_back(g_mons[g_set.displayIndex].rect);
  }

  for (auto& r : targets) {
    if (g_winInset > 0) {
      r.left += g_winInset; r.top += g_winInset;
      r.right -= g_winInset; r.bottom -= g_winInset;
    }
    auto* w = new LpD3DWindow();
    std::string e;
    if (!lp_d3d_create(g_hInst, r, w, &e)) {
      if (err) *err = e;
      delete w;
      destroyOverlays();
      return false;
    }
    if (!lp_d3d_set_shaders(w, LP_HLSL_VS, LP_HLSL_FS, &e)) {
      if (err) *err = e;
      lp_d3d_destroy(w);
      delete w;
      destroyOverlays();
      return false;
    }
    w->wantReadback = g_selfTestMode;
    w->skipPresent = g_noPresent;
    g_overlays.push_back(w);
  }
  return !g_overlays.empty();
}

/* 把当前设置 + 音频包络打包成 uniform */
static void fillParams(LpD3DWindow* w, LpParams* p) {
  memset(p, 0, sizeof(*p));
  const int ti = (g_set.themeIndex >= 0 && g_set.themeIndex < LP_THEME_COUNT)
                     ? g_set.themeIndex : 0;
  const LpTheme& th = LP_THEMES[ti];

  p->P0[0] = (float)w->w;
  p->P0[1] = (float)w->h;
  p->P0[2] = (float)g_set.styleMode;
  p->P0[3] = g_set.beamInset;

  p->P1[0] = g_set.audioIntensity;
  p->P1[1] = g_set.responseRange;
  p->P1[2] = g_audio.idleWave;
  p->P1[3] = g_set.alphaK;

  p->P2[0] = g_audio.bands.subBass;
  p->P2[1] = g_audio.bands.bass;
  p->P2[2] = g_audio.bands.lowMid;
  p->P2[3] = g_audio.bands.mid;
  p->P3[0] = g_audio.bands.highMid;
  p->P3[1] = g_audio.bands.energy;
  p->P3[2] = g_audio.bands.presence;
  p->P3[3] = g_audio.bands.brilliance;
  p->P4[0] = g_audio.bands.air;
  p->P4[1] = g_audio.bands.brightness;
  p->P4[2] = th.glow;
  p->P4[3] = g_set.peakEnabled ? 1.0f : 0.0f;
  p->P5[0] = g_set.peakIntensity;
  p->P5[1] = (float)g_time;

  memcpy(p->cool, th.cool, sizeof(th.cool));
  memcpy(p->warm, th.warm, sizeof(th.warm));
  memcpy(p->peak, th.peak, sizeof(th.peak));
}

/* ============================ 托盘 ============================ */

static void showPanel();
static void syncPanelControls();
/* 预设：建面板时要用 refreshPresetOptions 填下拉，所以先声明。 */
static void loadPresets();
static void refreshPresetOptions();

static void showTrayMenu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, ID_OPEN_PANEL, L"调节面板…");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  HMENU disp = CreatePopupMenu();
  AppendMenuW(disp, MF_STRING | (g_set.displayIndex < 0 ? MF_CHECKED : 0),
              ID_DISPLAY_ALL, L"全部显示器");
  for (size_t i = 0; i < g_mons.size(); i++) {
    wchar_t text[64];
    swprintf(text, 64, L"显示器 %d%s", (int)i + 1, g_mons[i].primary ? L"（主屏）" : L"");
    AppendMenuW(disp, MF_STRING | (g_set.displayIndex == (int)i ? MF_CHECKED : 0),
                ID_DISPLAY_BASE + (int)i, text);
  }
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)disp, L"显示在");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ID_QUIT, L"退出");

  POINT pt;
  GetCursorPos(&pt);
  /* 托盘菜单必须先 SetForegroundWindow，否则点空白处菜单不会消失 */
  SetForegroundWindow(hwnd);
  const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, 0, hwnd, nullptr);
  PostMessageW(hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu);

  if (cmd == ID_OPEN_PANEL) {
    showPanel();
  } else if (cmd == ID_QUIT) {
    g_quitting = true;
    PostQuitMessage(0);
  } else if (cmd == ID_DISPLAY_ALL) {
    g_set.displayIndex = -1;
    std::string err;
    if (!buildOverlays(&err))
      MessageBoxW(nullptr, (L"切换显示器失败：\n" + std::wstring(err.begin(), err.end())).c_str(),
                  LP_APP_NAME, MB_ICONERROR);
    saveSettings();
  } else if (cmd >= ID_DISPLAY_BASE && cmd < ID_DISPLAY_BASE + 100) {
    g_set.displayIndex = cmd - ID_DISPLAY_BASE;
    std::string err;
    if (!buildOverlays(&err))
      MessageBoxW(nullptr, (L"切换显示器失败：\n" + std::wstring(err.begin(), err.end())).c_str(),
                  LP_APP_NAME, MB_ICONERROR);
    saveSettings();
  }
}

static LRESULT CALLBACK trayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_LP_TRAY) {
    if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) showTrayMenu(h);
    else if (l == WM_LBUTTONUP) showPanel();
    return 0;
  }
  /* 全局热键退出。存在的理由很实际：覆盖层万一吃掉了鼠标，
     托盘就点不到了，用户得有办法从键盘上把它关掉。 */
  if (m == WM_HOTKEY && w == HOTKEY_QUIT) {
    g_quitting = true;
    PostQuitMessage(0);
    return 0;
  }
  if (m == WM_DESTROY) return 0;
  return DefWindowProcW(h, m, w, l);
}

/* 托盘消息窗口 + 键盘退路热键。
 *
 * ⚠ 顺序很要紧：这一步必须在**覆盖层创建之前**跑完。
 * 覆盖层是置顶全屏的，一旦它把鼠标吃掉（历史上真出过这个 bug），
 * 托盘图标就点不到了 —— 那时用户唯一的出路就是键盘。
 * 所以热键不能等托盘图标装好才注册，得抢在覆盖层出现之前。 */
static bool createTrayWindow() {
  if (g_trayHwnd) return true;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = trayProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = LP_WND_CLASS_TRAY;
  RegisterClassExW(&wc);

  /* 消息专用窗口：不显示、不占 Z 序，只用来收托盘回调 */
  g_trayHwnd = CreateWindowExW(0, LP_WND_CLASS_TRAY, LP_APP_NAME, 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, g_hInst, nullptr);
  if (!g_trayHwnd) return false;

  /* 键盘退路：Ctrl+Alt+Q 直接退出。覆盖层是置顶全屏的，
     万一它吃掉了鼠标，用户至少还能从键盘上把它关掉。 */
  RegisterHotKey(g_trayHwnd, HOTKEY_QUIT, MOD_CONTROL | MOD_ALT, 'Q');
  return true;
}

static bool createTray() {
  if (!createTrayWindow()) return false;

  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_trayHwnd;
  nid.uID = ID_TRAY_ICON;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_LP_TRAY;
  nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                GetSystemMetrics(SM_CXSMICON),
                                GetSystemMetrics(SM_CYSMICON), 0);
  if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  lstrcpynW(nid.szTip, L"光柱 · 左键开面板，右键菜单，Ctrl+Alt+Q 退出", 128);
  g_trayOk = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
  return g_trayOk;
}

static void destroyTray() {
  if (!g_trayHwnd) return;
  UnregisterHotKey(g_trayHwnd, HOTKEY_QUIT);
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_trayHwnd;
  nid.uID = ID_TRAY_ICON;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  DestroyWindow(g_trayHwnd);
  g_trayHwnd = nullptr;
}

/* ============================ 调节面板 ============================ */
/*
 *  面板本体是**自绘**的（见 panel.cpp，外观对齐参考实现的浏览器版面板）。
 *  这里只负责三件事：
 *    ① 把设置项翻译成面板项（绑定到 g_set 的字段，以及几个代理量）；
 *    ② 接收面板回调，做副作用（重建覆盖层 / 重连音频 / 保存配置）；
 *    ③ 给托盘与自检提供 showPanel() / syncPanelControls() 两个入口。
 */

static const int ID_PANEL_RESET = 9001;       // 「恢复默认设置」按钮
static const int ID_PANEL_RECONNECT = 9002;   // 「重新连接音频设备」按钮

static std::vector<LpPanelItem> g_panelItems;
static bool g_panelReady = false;
static bool g_settingsDirty = false;

static LpPanelItem& addItem(LpItemKind kind, int id, int tab, const wchar_t* label) {
  LpPanelItem it;
  it.kind = kind;
  it.id = id;
  it.tab = tab;
  it.label = label ? label : L"";
  g_panelItems.push_back(it);
  return g_panelItems.back();
}

/* 滑杆：范围一律照抄浏览器版面板，注释里写的就是界面上的 min..max */
static void addSlider(int id, int tab, const wchar_t* label, float* v,
                      float lo, float hi, float step, float deflt,
                      int dec, const wchar_t* unit, float dispMul = 1.f) {
  LpPanelItem& it = addItem(LP_SLIDER, id, tab, label);
  it.fval = v;
  it.minv = lo; it.maxv = hi; it.step = step; it.deflt = deflt;
  it.decimals = dec;
  it.unit = unit ? unit : L"";
  it.dispMul = dispMul;
}

static void addSwitch(int id, int tab, const wchar_t* label, bool* v, bool deflt) {
  LpPanelItem& it = addItem(LP_SWITCH, id, tab, label);
  it.bval = v;
  it.deflt = deflt ? 1.f : 0.f;
}

static void addSelect(int id, int tab, const wchar_t* label, int* v, float deflt,
                      const std::vector<std::wstring>& opts) {
  LpPanelItem& it = addItem(LP_SELECT, id, tab, label);
  it.ival = v;
  it.deflt = deflt;
  it.options = opts;
}

static void addSegment(int id, int tab, const wchar_t* label, int* v, float deflt,
                       const std::vector<std::wstring>& opts) {
  LpPanelItem& it = addItem(LP_SEGMENT, id, tab, label);
  it.ival = v;
  it.deflt = deflt;
  it.options = opts;
}

static void addHint(int tab, std::initializer_list<const wchar_t*> lines) {
  LpPanelItem& it = addItem(LP_HINT, 0, tab, L"");
  for (const wchar_t* s : lines) it.options.push_back(s);
}

/* 把上游工程配置里的主题列表搬过来。顺序必须与 LP_THEMES 一致 ——
   第 0 项是「自动轮询」占位，1..N 对应 LP_THEMES[0..N-1]。 */
static std::vector<std::wstring> themeOptionLabels() {
  std::vector<std::wstring> v;
  v.push_back(L"自动轮询");
  for (int i = 0; i < LP_THEME_COUNT; i++) {
    wchar_t buf[96] = {};
    MultiByteToWideChar(CP_UTF8, 0, LP_THEMES[i].name, -1, buf, 96);
    v.push_back(buf);
  }
  return v;
}

/* 显示器列表：第 0 项「全部显示器」，1..N 对应 g_mons[0..N-1] */
static std::vector<std::wstring> displayOptionLabels() {
  std::vector<std::wstring> v;
  v.push_back(L"全部显示器");
  for (size_t k = 0; k < g_mons.size(); k++) {
    wchar_t buf[64] = {};
    swprintf(buf, 64, L"显示器 %d%s", (int)k + 1, g_mons[k].primary ? L"（主屏）" : L"");
    v.push_back(buf);
  }
  return v;
}

static void buildPanelItems() {
  g_panelItems.clear();
  g_panelItems.reserve(64);

  /* ======================= 声音页（tab 0）======================= */
  addItem(LP_SECTION, 0, 0, L"声音来源");
  addItem(LP_METER, 0, 0, L"输入电平");
  addSegment(S_SOURCE, 0, L"音频输入", &g_sourceSel, 0,
             {L"系统声音（环回）", L"空闲（不接输入）"});
  addItem(LP_BUTTON, ID_PANEL_RECONNECT, 0, L"重新连接音频设备");
  addHint(0, {L"抓的是「默认播放设备」的输出，不需要选设备；",
              L"换过耳机 / 声卡之后点上面的按钮重连。"});

  addSelect(S_FPS, 0, L"渲染帧率（桌面版）", &g_fpsSel, 1,
            {L"30 fps", L"60 fps", L"120 fps", L"144 fps"});
  /* ★ 这段文案改过三次，每次都对应一次认知修正，留着当路标。
     第一版：「参考实现按 60fps 调校，改帧率手感就不一致」—— 错。
     第二版（v5）：「推送频率跟着帧率走、平滑系数做等效换算」—— 也错。
     第三版（v6 初）：以为分析层跟渲染帧率跑（`getAudioData` 在 rAF 里）—— 还是错。
     真相（v6 定稿）：参考实现有**三层各自的频率**（见 audio.cpp 的 tick()）：
       · 频谱层固定 30Hz —— `桥接层` 的 `PUSH_HZ = 30`，
         滚动直方图挂在它上面，窗口永远是「秒 × 30」帧；
       · 分析层固定 **250Hz** —— bundle 里 `setInterval(ht, 1e3/1e3)`，
         请求 1ms、Chromium 钳到 4ms，实测 249.6 次/秒。平滑系数
         `ee = 0.15 / 0.08` 是「每次调用作用一次」、**不按 dt 缩放**，
         所以这个频率直接决定包络时间常数（250Hz→24.6ms，60Hz→111ms）；
       · 渲染层 = `general.fps` —— `<Canvas frameloop="demand">` +
         `iG fpsLimit={e}`，`e` 来自 `applyGeneralProperties({fps})`。
     任何「等效换算」都会把响应幅度改小（v5 把它改到了 67%）。
     所以这一项（渲染帧率）只该管画面重画多快，别去动另外两层。 */
  addHint(0, {L"只改「画面每秒重画几次」，与参考实现的 general.fps 同义。",
              L"频谱 30Hz、分析 250Hz 各有自己的时钟。",
              L"60 fps = 参考实现默认值；更高只是更顺，幅度不变。"});

  addSelect(S_ANA_HZ, 0, L"音频分析频率", &g_anaSel, 3,
            {L"30 Hz", L"60 Hz", L"120 Hz", L"250 Hz（桌面版实测）"});
  /* ★ 这一项是 v6 新加的，也是本轮「原生响应幅度不如桌面版」的根因所在。
     参考实现把音频分析（getAudioData，含一阶平滑 ee=0.15）放在
     `setInterval(ht, 1e3/1e3)` 里 —— 请求 1ms，Chromium 把嵌套定时器
     钳到 4ms，实测 **249.6 次/秒**。而 ee 是「每次调用作用一次」、
     不按 dt 缩放的，所以这个频率**就是**包络的时间常数：
       250Hz → τ=24.6ms（桌面版）
        60Hz → τ=111ms（慢 4.5 倍 → 幅度被压小、弱拍被抹平）
     之前移植版把它挂在渲染帧率上，就是这个原因。
     ⚠ 它和「渲染帧率」是两条独立的时钟，别设成同一个值就以为对齐了。 */
  addHint(0, {L"参考实现把分析放在 1ms 定时器里，实测 250 次/秒。",
              L"调低 → 光柱变迟钝、鼓点幅度变小。",
              L"250 Hz 才是桌面版观感；它只影响包络跟手程度。"});

  addItem(LP_SECTION, 0, 0, L"音频响应校准");
  addSlider(S_GAIN_DB, 0, L"增益", &g_set.gainDb, -12, 12, 0.5f, 0, 1, L" dB");
  addSlider(S_WIDTH_DB, 0, L"动态范围", &g_set.widthDb, 15, 60, 1, 46, 0, L" dB");
  addSlider(S_BALANCE, 0, L"频谱平衡", &g_set.balance, 0, 1, 0.02f, 0.85f, 0, L"%", 100.f);
  addSegment(S_MAP_MODE, 0, L"电平映射", &g_set.mapMode, 0,
             {L"每段自适应", L"共用窗口"});
  addSegment(S_BAND_MODE, 0, L"频率轴", &g_set.bandMode, 0,
             {L"等宽 0–16k", L"等比 30–16k"});
  addSlider(S_LEVEL, 0, L"总电平", &g_set.level, 0.05f, 1.0f, 0.01f, 0.15f, 2, L"");
  addSlider(S_CONTRAST, 0, L"对比度", &g_set.contrast, 1, 3, 0.1f, 1.8f, 1, L"×");
  addSlider(S_LO_PCT, 0, L"常态分位", &g_set.loPct, 0.02f, 0.5f, 0.01f, 0.20f, 0, L"%", 100.f);
  addSlider(S_HI_PCT, 0, L"峰值分位", &g_set.hiPct, 0.8f, 1.0f, 0.01f, 0.99f, 0, L"%", 100.f);
  addSlider(S_WIN_SEC, 0, L"跟踪窗口", &g_set.rpWinSec, 3, 30, 1, 10, 0, L" 秒");
  addHint(0, {L"「每段自适应」下：顶满就调低「总电平」，",
              L"鼓点不够跳就加大「对比度」。",
              L"「常态分位」调高 → 常态更暗；",
              L"「峰值分位」调低 → 易冲满格。",
              L"「增益 / 动态范围 / 频谱平衡」只在共用窗口下生效。"});
  /* ⚠ 提示行的宽度上限是 28 个汉字（面板 320px、字号 10.5px、可用 296px）。
     面板是自绘的，提示用 `StringTrimmingEllipsisCharacter + NoWrap` ——
     超宽会**静默截断**，既不换行也不报警，画面上就是半行字被切掉。
     上面这几行原来有三行超宽（74/66/58 半宽），看着像面板坏了。
     改文案后用 `python tools/check_hints.py` 量一遍，别再靠感觉。 */

  /* ======================= 参数页（tab 1）======================= */
  /* 顺序照参考实现：先「恢复默认设置」，紧接着**预设区**
     （参考实现的注释：「它管的是整套参数，比下面任何一条具体参数都更靠前」），
     然后才是各个分组。 */
  addItem(LP_BUTTON, ID_PANEL_RESET, 1, L"恢复默认设置");

  addItem(LP_SECTION, 0, 1, L"预设");
  /* 记下这个下拉在 g_panelItems 里的下标：预设增删时要原地改它的 options，
     不能重建面板（面板只持有 items 向量的指针，重建会留下一堆悬空引用）。 */
  g_presetSelItem = (int)g_panelItems.size();
  addSelect(S_PRESET_SEL, 1, L"预设", &g_presetSel, 0, {L"（未选择）"});
  addItem(LP_BUTTON, S_PRESET_SAVE, 1, L"保存参数");
  addItem(LP_BUTTON, S_PRESET_RENAME, 1, L"重命名");
  addItem(LP_BUTTON, S_PRESET_DEL, 1, L"删除预设");
  addHint(1, {L"一套预设含「声音」页的参数，两页一起存。",
              L"选中一个预设就载入它；再点「保存参数」即覆盖。",
              L"预设不改「显示在哪块屏」和「声音来源」。"});
  refreshPresetOptions();

  addItem(LP_SECTION, 0, 1, L"外观");
  addSelect(S_STYLE, 1, L"光柱风格", &g_set.styleMode, 0,
            {L"光柱跳动", L"常驻光柱 · 节拍闪色"});
  addSelect(S_THEME, 1, L"颜色主题", &g_themeSel, 1, themeOptionLabels());
  addSlider(S_THEME_CYCLE, 1, L"轮询间隔", &g_set.themeCycleSec, 10, 300, 10, 60, 0, L" 秒");
  addSlider(S_BEAM_INSET, 1, L"光柱离边距离", &g_set.beamInset, -0.15f, 0.30f, 0.01f, 0, 2, L"");

  addItem(LP_SECTION, 0, 1, L"音频响应");
  addSlider(S_AUDIO_INT, 1, L"音频响应强度", &g_set.audioIntensity, 0.3f, 2.5f, 0.1f, 1, 1, L"");
  addSlider(S_RESP_RANGE, 1, L"响应范围", &g_set.responseRange, 0.3f, 2.0f, 0.1f, 1, 1, L"");
  addSwitch(S_PEAK_EN, 1, L"启用强调色", &g_set.peakEnabled, true);
  addSlider(S_PEAK_INT, 1, L"强调色强度", &g_set.peakIntensity, 0, 2, 0.1f, 1, 1, L"");

  addItem(LP_SECTION, 0, 1, L"效果 · 空闲波浪");
  addSwitch(S_IDLE_EN, 1, L"启用空闲波浪", &g_set.idleEnabled, true);
  addSlider(S_IDLE_DEBOUNCE, 1, L"防抖", &g_set.idleDebounce, 0.5f, 5, 0.5f, 1, 1, L" 秒");
  addSlider(S_IDLE_FADE, 1, L"过渡", &g_set.idleFade, 0.5f, 5, 0.5f, 1, 1, L" 秒");

  addItem(LP_SECTION, 0, 1, L"桌面版");
  addSlider(S_ALPHA_K, 1, L"光柱实心度", &g_set.alphaK, 0, 1, 0.05f, 0.10f, 2, L"");
  addSelect(S_DISPLAY, 1, L"显示在", &g_displaySel, 0, displayOptionLabels());
  addHint(1, {L"「光柱实心度」0 = 纯加光（像真光打在桌面上）。",
              L"1 = 实心覆盖，任何壁纸下都清晰。",
              L"右键任意一项 = 把这一项恢复默认值。"});
}

static void applyAudioSource() {
  if (g_set.audioIdleMode) {
    g_audio.stop();
    g_audioOk = false;
    g_audioErr = "已切到「空闲」（不接输入）";
    return;
  }
  if (g_audio.running()) return;
  if (!g_audio.start()) {
    g_audioErr = g_audio.lastError() ? g_audio.lastError() : "未知错误";
    g_audioOk = false;
  } else {
    g_audioOk = true;
    g_audioErr.clear();
  }
}

/* ============================ 参数预设 ============================
 * 对应参考实现面板**参数页**上那一块（桥接层 的「预设区」）：
 * 一个下拉 + 「保存参数」+「删除」，外加一个名称输入框（这里拆成「重命名」按钮）。
 *
 * ⚠ 一套预设包含**两页**的参数。参考实现特意在界面上写了一句话提醒用户，
 *   因为「声音页的参数也进去了」这件事看不出来，用户很容易以为预设只管当前页。
 *   这里直接存 visitAllSettings 的整份清单，所以天然覆盖两页。
 *
 * 存放：%APPDATA%\LightPillar\presets.ini，一个预设一个 section。
 * 为什么和 settings.ini 分开：生命周期不同 —— 配置只有一份、随时被覆盖；
 * 预设是多份、由用户显式增删。
 */
struct LpPreset {
  std::wstring name;
  LpKvList kv;
};

static std::vector<LpPreset> g_presets;

/* 下拉的取值约定：0 = 未选择，1..N = g_presets[k-1]，N+1 = 「＋ 新建预设…」 */
static int presetNewOptionIndex() { return (int)g_presets.size() + 1; }

static int presetIndexOfName(const std::wstring& n) {
  for (size_t i = 0; i < g_presets.size(); i++) {
    if (g_presets[i].name == n) return (int)i;
  }
  return -1;
}

/* 当前参数是不是**正好等于**这一套预设？
 *
 * 用来决定「启动后下拉该不该停在它上面」。
 * 为什么要判：下拉的意思是「现在用的是哪一套」。如果只按记下来的编号恢复选择，
 * 而下拉显示的那一套其实和内存里的参数不一致，下拉就是在说谎 ——
 * 用户看到「明明选中了某个预设，参数却是别的/是默认值」，只能得出
 * 「预设没生效」这一个结论。宁可显示「（未选择）」也不要显示错的。
 *
 * ⚠ 比较时跳过 displayIndex / audioIdleMode —— 那两项**不跟着预设走**
 *   （见 presetApply 的注释），预设文件里存的是存档当时的值，
 *   拿它参与比较会导致「刚载入的预设立刻显示成未选中」。
 */
static bool presetMatchesCurrent(int idx) {
  if (idx < 0 || idx >= (int)g_presets.size()) return false;
  if (g_presets[idx].kv.empty()) return false;

  LpKvList cur;
  PresetIo io(&cur);
  io.saving = true;
  visitAllSettings(io);   // 把当前 g_set 摊平成同一张 kv，再逐键比字符串
  for (const auto& kv : cur) {
    if (kv.first == L"displayIndex" || kv.first == L"audioIdleMode") continue;
    const std::wstring* o = nullptr;
    for (const auto& p : g_presets[idx].kv) {
      if (p.first == kv.first) { o = &p.second; break; }
    }
    if (!o || *o != kv.second) return false;
  }
  return true;
}

static void loadPresets() {
  g_presets.clear();
  g_presetSel = 0;
  const std::wstring p = presetPath();
  const int n = (int)GetPrivateProfileIntW(L"meta", L"count", 0, p.c_str());
  for (int i = 0; i < n && i < 500; i++) {
    wchar_t sec[32] = {};
    swprintf(sec, 32, L"preset%d", i);
    wchar_t name[128] = {};
    GetPrivateProfileStringW(sec, L"name", L"", name, 127, p.c_str());
    /* 没名字的残档直接跳过。兜的是「删预设时文件只写了一半」这种情况 ——
       不兜的话下拉里会出现一个「选了没反应」的空选项，那种静默失效最难查。 */
    if (!name[0]) continue;
    LpPreset ps;
    ps.name = name;
    /* ★ 从**文件**读进 kv。这里绝对不能用 PresetIo —— 它的读方向是 kv → g_set，
       而此刻 kv 还是空的，结果就是「把默认值写进 g_set」而不是「把文件读进 kv」。
       （v8 就是这么写的，后果见 PresetLoadIo 的注释。）
       这一行也绝不能碰 g_set：loadPresets 只负责把磁盘上的档位读进内存列表。 */
#ifdef LP_BUG_PRESETLOAD
    /* ★ 对照组：故意退回 v8 那个错误写法，用来证明自检**抓得住**这个 bug。
       抓不住 bug 的测试没有意义，所以这个分支要留着。
       编译：./build.sh bugpreset  → build/光柱-bugpreset.exe */
    PresetIo bio(&ps.kv);
    bio.saving = false;
    visitAllSettings(bio);
#else
    PresetLoadIo io(&ps.kv, sec, p);
    visitAllSettings(io);
#endif
    g_presets.push_back(std::move(ps));
  }
  /* 「上次用的是哪套」只在**当前参数确实等于它**时才恢复 ——
     否则下拉会显示一套其实没在用的预设（见 presetMatchesCurrent 的注释）。 */
  const int act = (int)GetPrivateProfileIntW(L"meta", L"active", -1, p.c_str());
  g_presetSel = (act >= 0 && act < (int)g_presets.size() && presetMatchesCurrent(act))
                    ? act + 1 : 0;
}

static void savePresetsToDisk() {
  const std::wstring p = presetPath();
  wchar_t b[32] = {};
  swprintf(b, 32, L"%d", (int)g_presets.size());
  WritePrivateProfileStringW(L"meta", L"count", b, p.c_str());
  /* 记住「上次用的哪个预设」，下次开面板下拉还是它 */
  swprintf(b, 32, L"%d", (g_presetSel >= 1 && g_presetSel <= (int)g_presets.size())
                             ? g_presetSel - 1 : -1);
  WritePrivateProfileStringW(L"meta", L"active", b, p.c_str());

  for (size_t i = 0; i < g_presets.size(); i++) {
    wchar_t sec[32] = {};
    swprintf(sec, 32, L"preset%d", (int)i);
    WritePrivateProfileStringW(sec, L"name", g_presets[i].name.c_str(), p.c_str());
    for (const auto& kv : g_presets[i].kv)
      WritePrivateProfileStringW(sec, kv.first.c_str(), kv.second.c_str(), p.c_str());
  }
  /* 删掉多出来的 section。删预设时如果只把 count 改小，旧 section 还留在文件里，
     下次 loadPresets 会按 count 读到正确数量，但文件会越积越脏。
     整段删（lpKeyName 传 NULL）比「删文件再重建」稳 —— 后者会踩到
     WritePrivateProfileString 的**进程内文件缓存**，删掉的文件可能被缓存写回来。 */
  for (int i = (int)g_presets.size(); i < (int)g_presets.size() + 16; i++) {
    wchar_t sec[32] = {};
    swprintf(sec, 32, L"preset%d", i);
    WritePrivateProfileStringW(sec, nullptr, nullptr, p.c_str());
  }
}

static void refreshPresetOptions() {
  /* 越界会让人从下拉里读到空字符串，所以先夹一下 */
  if (g_presetSel < 0 || g_presetSel > (int)g_presets.size()) g_presetSel = 0;
  if (g_presetSelItem < 0 || g_presetSelItem >= (int)g_panelItems.size()) return;
  std::vector<std::wstring>& o = g_panelItems[g_presetSelItem].options;
  o.clear();
  o.push_back(L"（未选择）");
  for (const auto& p : g_presets) o.push_back(p.name);
  o.push_back(L"＋ 新建预设…");
  lp_panel_sync();
}

/* 把当前 g_set 存成预设。名字已存在就**覆盖**（顺序不变，和参考实现一致）。 */
static void presetSaveAs(const std::wstring& name) {
  LpPreset ps;
  ps.name = name;
  PresetIo io(&ps.kv);
  io.saving = true;
  visitAllSettings(io);
  const int k = presetIndexOfName(name);
  if (k >= 0) g_presets[k] = std::move(ps);
  else g_presets.push_back(std::move(ps));
  g_presetSel = ((k >= 0) ? k : (int)g_presets.size() - 1) + 1;
  savePresetsToDisk();
  refreshPresetOptions();
}

/* 把预设套回 g_set，并让整条链路跟上。 */
static void presetApply(int idx) {
  if (idx < 0 || idx >= (int)g_presets.size()) return;

  /* ★ 空 kv 的档位一律拒绝套用。这不是理论上的防御 —— 真发生过：
     kv 没被填上的档位套下去，会把整套参数冲成出厂默认值。
     宁可什么都不做并明说，也不要静默地把用户的参数清掉。 */
  if (g_presets[idx].kv.empty()) {
    lp_panel_set_status(L"预设「" + g_presets[idx].name + L"」是空的，已跳过");
    return;
  }

  /* 预设**不动**这两项：「显示在哪块屏」和「声音来源」是「这台机器怎么用」，
     不是参考实现参数 —— 和「恢复默认设置」的处理一致（那里也特意保留了它们）。
     做法是套完再写回去，这样 visitAllSettings 仍然只需要维护一份清单。 */
  const int keepDisplay = g_set.displayIndex;
  const int keepIdle = g_set.audioIdleMode;

  PresetIo io(&g_presets[idx].kv);
  io.saving = false;
  visitAllSettings(io);

  g_set.displayIndex = keepDisplay;
  g_set.audioIdleMode = keepIdle;
  clampSettings();   // 预设文件是用户能改的文本，同样不可信

  /* 一律走「重档」副作用：一套预设可能同时改了频段划分（bandMode）和
     分位定义（loPct / hiPct / rpWinSec），必须 resetTrackers() 重建直方图 ——
     否则会出现「预设看着载入了、画面却和以前一样」这种极难查的静默失效。 */
  syncPanelControls();
  pushAudioParams();
  applyAudioSource();
  lp_panel_sync();
  /* 记住「上次用的是哪套」（写进 meta/active），下次开面板下拉还停在它上面。
     不写的话「选一下就退出」下次回来下拉是空的 —— 参数其实已经生效了，
     只是界面不承认，看着像没保存。 */
  savePresetsToDisk();
  g_settingsDirty = true;
}

static void presetDelete(int idx) {
  if (idx < 0 || idx >= (int)g_presets.size()) return;
  g_presets.erase(g_presets.begin() + idx);
  /* 选择位要跟着挪：删的是选中项 → 回到「未选择」；
     删的是它前面的 → 选中项下标整体前移一位。 */
  if (g_presetSel == idx + 1) g_presetSel = 0;
  else if (g_presetSel > idx + 1) g_presetSel--;
  savePresetsToDisk();
  refreshPresetOptions();
}

static bool presetHasSelection() {
  return g_presetSel >= 1 && g_presetSel <= (int)g_presets.size();
}

/* 弹输入框给新预设起名。没有常驻的名称输入框，所以「新建」时才问 ——
   比常驻一个空框更省地方，也不会出现「框里有名字但没点保存」的悬空状态。 */
static void presetPromptNew() {
  if (lp_prompt_is_open()) return;
  lp_prompt_open(g_panel, L"保存为新预设", L"", [](bool ok, std::wstring name) {
    if (!ok) { refreshPresetOptions(); return; }
    presetSaveAs(name);
    lp_panel_set_status(L"已保存预设「" + name + L"」（视觉 + 声音）");
    refreshPresetOptions();
  });
}

static void presetSaveClicked() {
  if (presetHasSelection()) {
    const std::wstring nm = g_presets[g_presetSel - 1].name;
    presetSaveAs(nm);
    lp_panel_set_status(L"已覆盖预设「" + nm + L"」");
    return;
  }
  presetPromptNew();
}

static void presetRenameClicked() {
  if (!presetHasSelection()) {
    lp_panel_set_status(L"先在上面选一个预设，或选「＋ 新建预设…」");
    return;
  }
  const int idx = g_presetSel - 1;
  const std::wstring old = g_presets[idx].name;
  if (lp_prompt_is_open()) return;
  lp_prompt_open(g_panel, L"重命名预设", old, [idx, old](bool ok, std::wstring name) {
    if (!ok || name == old) return;
    const int dup = presetIndexOfName(name);
    if (dup >= 0 && dup != idx) {
      lp_panel_set_status(L"已经有叫「" + name + L"」的预设了");
      return;
    }
    g_presets[idx].name = name;
    savePresetsToDisk();
    refreshPresetOptions();
    lp_panel_set_status(L"已重命名为「" + name + L"」");
  });
}

static void presetDeleteClicked() {
  if (!presetHasSelection()) {
    lp_panel_set_status(L"先在上面选一个预设，再点删除");
    return;
  }
  const int idx = g_presetSel - 1;
  const std::wstring nm = g_presets[idx].name;
  presetDelete(idx);
  lp_panel_set_status(L"已删除预设「" + nm + L"」");
}

static void presetSelectChanged() {
  if (g_presetSel <= 0) {
    savePresetsToDisk();   // 记住「当前没选」
    lp_panel_set_status(L"未选择预设");
    return;
  }
  if (g_presetSel == presetNewOptionIndex()) {
    /* 选「＋ 新建预设…」= 把**当前**参数存成一个新预设，所以要先问名字。
       问之前先把选择位退回「未选择」—— 否则下拉会一直把「＋ 新建预设…」
       当成当前值显示，看着像已经选了个叫这个名字的预设。 */
    g_presetSel = 0;
    refreshPresetOptions();
    presetPromptNew();
    return;
  }
  const int idx = g_presetSel - 1;
  presetApply(idx);
  lp_panel_set_status(L"已载入预设「" + g_presets[idx].name + L"」（视觉 + 声音）");
}

/* 把当前 g_set 打出来。`--showcfg` 用，也是排查「我的设置去哪了」的唯一手段 ——
   面板显示的是内存里的值，光看面板看不出「启动时被哪一步改过」。 */
static void dumpSettings(const char* tag) {
  printf("[cfg] %s\n", tag);
  printf("[cfg]   ini     = %ls\n", iniPath().c_str());
  printf("[cfg]   presets = %ls\n", presetPath().c_str());
  printf("[cfg]   styleMode=%d beamInset=%.4f themeIndex=%d themeAuto=%d fpsLimit=%d anaHz=%d\n",
         g_set.styleMode, (double)g_set.beamInset, g_set.themeIndex,
         g_set.themeAuto ? 1 : 0, g_set.fpsLimit, g_set.anaHz);
  printf("[cfg]   audioIntensity=%.4f responseRange=%.4f contrast=%.4f level=%.4f\n",
         (double)g_set.audioIntensity, (double)g_set.responseRange,
         (double)g_set.contrast, (double)g_set.level);
  printf("[cfg]   gainDb=%.3f widthDb=%.3f balance=%.4f bandMode=%d mapMode=%d\n",
         (double)g_set.gainDb, (double)g_set.widthDb, (double)g_set.balance,
         g_set.bandMode, g_set.mapMode);
  printf("[cfg]   loPct=%.4f hiPct=%.4f rpWinSec=%.3f alphaK=%.4f\n",
         (double)g_set.loPct, (double)g_set.hiPct, (double)g_set.rpWinSec,
         (double)g_set.alphaK);
  printf("[cfg]   displayIndex=%d audioIdleMode=%d 预设数=%d 当前选择=%d\n",
         g_set.displayIndex, g_set.audioIdleMode, (int)g_presets.size(), g_presetSel);
  fflush(stdout);
}

static void panelNotify(int id, void* user) {
  (void)user;
  if (id == ID_PANEL_RESET) {
    /* 「恢复默认」不该把「声音来源 / 显示在哪块屏」一起改掉 —— 那两项是
       用户对**这台机器**的选择，不是参考实现参数。 */
    const int keepIdle = g_set.audioIdleMode;
    const int keepDisplay = g_set.displayIndex;
    g_set = LpSettings{};
    g_set.audioIdleMode = keepIdle;
    g_set.displayIndex = keepDisplay;
    /* 已经离开任何一套预设了，下拉不该还显示着它（参考实现同款处理）。 */
    g_presetSel = 0;
    savePresetsToDisk();
    refreshPresetOptions();
    pushAudioParams();
    applyAudioSource();
    std::string e;
    buildOverlays(&e);
  } else if (id == ID_PANEL_RECONNECT) {
    applyAudioSource();
  } else if (id == S_THEME) {
    if (g_themeSel <= 0) {
      g_set.themeAuto = true;
    } else {
      g_set.themeAuto = false;
      g_set.themeIndex = g_themeSel - 1;
      g_themeClock = 0.0f;
    }
  } else if (id == S_DISPLAY) {
    g_set.displayIndex = g_displaySel - 1;
    std::string e;
    if (!buildOverlays(&e)) {
      std::wstring we(e.begin(), e.end());
      MessageBoxW(nullptr, (L"切换显示器失败：\n" + we).c_str(), LP_APP_NAME, MB_ICONERROR);
    }
  } else if (id == S_FPS) {
    g_set.fpsLimit = (g_fpsSel == 0) ? 30 : (g_fpsSel == 1) ? 60 : (g_fpsSel == 2) ? 120 : 144;
    /* ★ 渲染帧率 = 参考实现的 `general.fps`，只决定**画面每秒重画几次**。
       另外两层各有自己的时钟，都不受它影响：
         · 频谱层固定 30Hz（`桥接层` 的 `PUSH_HZ`），
           滚动直方图挂在它上面 → 这里**不需要** resetTrackers()，
           窗口长度和内容都与渲染帧率无关，改帧率不会打断分位归一化；
         · 分析层按 anaHz（默认 250，见 S_ANA_HZ）→ 也**不需要**换算
           任何平滑系数，ee 是「每次调用作用一次」的固定值。 */
  } else if (id == S_ANA_HZ) {
    g_set.anaHz = (g_anaSel == 0) ? 30 : (g_anaSel == 1) ? 60 : (g_anaSel == 2) ? 120 : 250;
    /* ⚠ 改分析频率**要**重置累加器（否则会按旧周期补跑一批分析），
       但**不要** resetTrackers()：直方图挂在 30Hz 的频谱层上，
       与分析频率无关，清空它会让分位归一化重新攒 10 秒。 */
    g_audio.anaHz = (double)g_set.anaHz;
    g_audio.resetAnalysisClock();
  } else if (id == S_SOURCE) {
    g_set.audioIdleMode = (g_sourceSel == 1) ? 1 : 0;
    applyAudioSource();
  } else if (id == S_PRESET_SEL) {
    presetSelectChanged();
  } else if (id == S_PRESET_SAVE) {
    presetSaveClicked();
  } else if (id == S_PRESET_RENAME) {
    presetRenameClicked();
  } else if (id == S_PRESET_DEL) {
    presetDeleteClicked();
  } else if (isLightAudioParam(id)) {
    /* 纯数值参数：重算 dB 窗口 / 倾斜表就够，**不要**动滚动直方图 */
    pushAudioParamsLight();
  } else if (id >= S_GAIN_DB && id <= S_WIN_SEC) {
    pushAudioParams();
  } else if (id == S_IDLE_EN) {
    g_audio.idleEnabled = g_set.idleEnabled;
  } else if (id == S_IDLE_DEBOUNCE) {
    g_audio.idleDebounce = g_set.idleDebounce;
  } else if (id == S_IDLE_FADE) {
    g_audio.idleFade = g_set.idleFade;
  }
  /* 拖动滑杆时每一项都会回调一次，逐次写 INI 太重（一次 20 条
     WritePrivateProfileString）。改成打脏标记，由主循环每秒落一次盘，
     退出前再强制落一次。 */
  g_settingsDirty = true;
  syncPanelControls();
}

static void showPanel() {
  if (!g_panelReady) {
    buildPanelItems();
    if (!lp_panel_open(g_hInst, L"光柱 · 调节面板", &g_panelItems, panelNotify, nullptr)) {
      MessageBoxW(nullptr, L"创建调节面板失败。", LP_APP_NAME, MB_ICONERROR);
      return;
    }
    g_panelReady = true;
    g_panel = lp_panel_hwnd();
  }
  syncPanelControls();
  lp_panel_show();
}

static void syncPanelControls() {
  /* 代理量与真实状态对齐。主题下拉的 0 是「自动轮询」，显示器下拉的 0 是「全部」。 */
  g_themeSel = g_set.themeAuto ? 0 : (g_set.themeIndex + 1);
  g_displaySel = g_set.displayIndex + 1;
  g_fpsSel = (g_set.fpsLimit == 30) ? 0 : (g_set.fpsLimit == 60) ? 1
             : (g_set.fpsLimit == 120) ? 2 : 3;
  g_anaSel = (g_set.anaHz == 30) ? 0 : (g_set.anaHz == 60) ? 1
             : (g_set.anaHz == 120) ? 2 : 3;
  g_sourceSel = g_set.audioIdleMode ? 1 : 0;
  lp_panel_sync();
}

static void destroyPanel() {
  lp_panel_close();
  g_panelReady = false;
  g_panel = nullptr;
}

/* ============================ 自检模式 ============================ */
/*
 *  光柱.exe --selftest <秒> <输出目录>
 *
 *  跑的是**真实主循环**（覆盖层 / 托盘 / 面板 / 音频全都真建），
 *  只是额外把可客观量化的东西落成文件：
 *    · 交换链回读的像素统计（最大 alpha / 最大 RGB / 亮区 bbox / 左右对称）
 *    · 覆盖层窗口属性（置顶 / 工具窗 / 无重定向表面 / 鼠标穿透）
 *    · 面板控件是否建出来、滑杆与主题下拉是否真的联动
 *    · 音频环回状态与电平
 *
 *  为什么必须回读、不能抓屏：DirectComposition 的内容抓屏抓不稳；而且
 *  alphaK=0.10 的加光最大 alpha 只有 11/255，截图缩略图上肉眼根本看不出。
 *  回读是唯一客观的口径。
 */

static double nowMs();   // 定义在下面「计时」一节
static void selftestClickThrough();

struct LpPixelStats {
  int maxAlpha = 0, maxRgb = 0;
  double sumAlpha = 0.0;
  /* ⚠ 必须是 long long：Windows 上 long 只有 32 位，而「全过程」累计采样数
     在 30 秒 × 50fps × 1600×900 下会到 2.2e9，直接溢出成负数。 */
  long long samples = 0, zeroAlpha = 0;
  int bl = INT_MAX, bt = INT_MAX, br = -1, bb = -1;
  double symMaxDiff = 0.0;
  long long symSamples = 0;

  double meanAlpha() const { return samples ? sumAlpha / (double)samples : 0.0; }
  bool bboxValid() const { return br >= bl && bb >= bt; }
};

/* 统计一帧 BGRA 缓冲。单独抽出来是为了能对「另存下来的某一帧」做同样的统计
   （见下面「换色帧取窗口内最亮一帧」）。 */
static void accumulatePixelsBuf(const unsigned char* base, int W, int H,
                                LpPixelStats* s) {
  if (!base || W <= 0 || H <= 0) return;
  for (int y = 0; y < H; y++) {
    const unsigned char* row = base + (size_t)y * (size_t)W * 4;
    for (int x = 0; x < W; x++) {
      const unsigned char* q = row + (size_t)x * 4;
      const int a = q[3];
      const int b = q[0], g = q[1], r = q[2];
      const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      s->samples++;
      s->sumAlpha += a;
      if (a == 0) s->zeroAlpha++;
      if (a > s->maxAlpha) s->maxAlpha = a;
      if (mx > s->maxRgb) s->maxRgb = mx;
      if (a > 3 || mx > 3) {
        if (x < s->bl) s->bl = x;
        if (x > s->br) s->br = x;
        if (y < s->bt) s->bt = y;
        if (y > s->bb) s->bb = y;
      }
    }
  }
  /* 左右对称：同一条水平线上 x 与 W-1-x 的 RGB 差。参考实现是镜像对称的，
     这一项掉下去就说明着色器或 uv 搞错了。 */
  for (int y = 0; y < H; y += 8) {
    const unsigned char* row = base + (size_t)y * (size_t)W * 4;
    for (int x = 0; x < W / 2; x += 4) {
      const unsigned char* p = row + (size_t)x * 4;
      const unsigned char* q = row + (size_t)(W - 1 - x) * 4;
      const int d = abs((int)p[0] - (int)q[0]) + abs((int)p[1] - (int)q[1]) +
                    abs((int)p[2] - (int)q[2]);
      s->symSamples++;
      if ((double)d > s->symMaxDiff) s->symMaxDiff = (double)d;
    }
  }
}

static void accumulatePixels(const LpD3DWindow* w, LpPixelStats* s) {
  if (w->lastFrame.empty() || w->w <= 0 || w->h <= 0) return;
  accumulatePixelsBuf(w->lastFrame.data(), w->w, w->h, s);
}

/* 一帧里最大的那个 RGB 分量。用来在一段窗口里挑「最亮的一帧」，
   比整帧统计便宜得多（只扫一遍，不做对称性比对）。 */
static int frameMaxRgb(const unsigned char* base, int W, int H) {
  if (!base || W <= 0 || H <= 0) return -1;
  int best = -1;
  const size_t n = (size_t)W * (size_t)H;
  for (size_t i = 0; i < n; i++) {
    const unsigned char* q = base + i * 4;
    const int b = q[0], g = q[1], r = q[2];
    const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx > best) best = mx;
  }
  return best;
}

/* 写 32bpp BMP。BMP 行天然 4 字节对齐，32bpp 不需要补 padding。
   alpha 一律写 255 —— 报告里 alpha 已经用数字给了，图片是给人看的，
   要能直接看出「光柱长什么样」。 */
static bool writeBmp32(const std::wstring& path, const unsigned char* bgra, int W, int H) {
  if (!bgra || W <= 0 || H <= 0) return false;
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;

  auto put32 = [](unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v & 0xff);         p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff); p[3] = (unsigned char)((v >> 24) & 0xff);
  };
  auto put16 = [](unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff);
  };

  const unsigned imgSize = (unsigned)W * 4u * (unsigned)H;
  unsigned char fh[14] = {'B', 'M'};
  put32(fh + 2, 54u + imgSize);
  put32(fh + 10, 54u);
  fwrite(fh, 1, 14, f);

  unsigned char ih[40] = {};
  put32(ih + 0, 40u);
  put32(ih + 4, (unsigned)W);
  put32(ih + 8, (unsigned)H);   // 正数 = 行自下而上
  put16(ih + 12, 1);
  put16(ih + 14, 32);
  put32(ih + 20, imgSize);
  fwrite(ih, 1, 40, f);

  std::vector<unsigned char> line((size_t)W * 4);
  for (int y = H - 1; y >= 0; y--) {
    const unsigned char* src = bgra + (size_t)y * (size_t)W * 4;
    for (int x = 0; x < W; x++) {
      line[(size_t)x * 4 + 0] = src[(size_t)x * 4 + 0];
      line[(size_t)x * 4 + 1] = src[(size_t)x * 4 + 1];
      line[(size_t)x * 4 + 2] = src[(size_t)x * 4 + 2];
      line[(size_t)x * 4 + 3] = 255;
    }
    fwrite(line.data(), 1, line.size(), f);
  }
  fclose(f);
  return true;
}

struct LpSelfTest {
  bool active = false;
  std::wstring outDir;
  double tStart = 0.0, tEnd = 8.0;

  /* 时间表 */
  double tPanel = 1.0;      // 建面板 + 测控件
  double tFidelity = 3.0;   // 强制 idle=1，取「保真帧」
  double tTheme = 5.0;      // 换主题，取「换色帧」
  double tShot = 4.0;       // 调 shot.exe 抓一张「覆盖层叠在真实桌面上」的实拍
  double tPanelShot = 9.0;  // 再抓一张「调节面板打开着」的实拍
  double tRegion = 12.0;    // 区域实验
  double tRegionNext = 0.0;
  int regionStage = 0;
  bool didPanel = false, didFidelity = false, didTheme = false, didShot = false;
  /* 保真帧那张 BMP 有没有真的写出去。报告里有像素统计、磁盘上没图，
     会被误读成「统计是编的」—— 所以结果显式落进报告。 */
  bool fidelityBmpOk = false;
  bool didPanelShot = false;
  bool shotLaunched = false, panelShotLaunched = false;
  bool panelCaptureOk = false;   // 面板自身的渲染缓冲有没有成功导出
  bool forceNext = false, forcedFrame = false;
  double themePixelAt = -1.0;
  /* 换色帧：**在换色后的一个短窗口里取最亮的一帧**，而不是拍某一帧。
     起因：单帧采样会被音频包络的谷点砸中 —— 同一个 exe、同一段测试音连跑三次，
     薄荷帧的「最大RGB」分别是 28 / 54 / 143，偶尔还会拍到近乎全黑的一帧，
     报告里看起来就像「一换主题就黑屏」。窗口取最亮后这个数稳定了，
     语义仍然是一帧（不是多帧累加），「亮区bbox / 左右色差」也才有意义。
     对比用的保真帧不受影响：它强制 idle=1，与音频无关，本来就稳定。 */
  double themeWindowEnd = -1.0;
  int themedBestMaxRgb = -1;
  std::vector<unsigned char> themedBest;   // 窗口内最亮那一帧的 BGRA

  long frames = 0;
  double fps = 0.0;
  LpPixelStats live, fidelity, themed;

  /* 音频：整轮里见过的最大电平与各频段峰值，用来证明「真有声音时会跳」 */
  float peakMeter = 0.0f;
  float peakSubBass = 0.0f, peakBass = 0.0f, peakMid = 0.0f;
  float peakPresence = 0.0f, peakBrilliance = 0.0f, peakAir = 0.0f;
  float peakEnergy = 0.0f;
  double firstSoundAt = -1.0;

  /* 面板测试结果 */
  int panelControls = 0;
  int panelSliders = 0;
  int sliderBefore = 0, sliderAfter = 0;
  bool sliderWorks = false;
  bool themeComboWorks = false;
  bool resetButtonWorks = false;
  int themeBefore = -1, themeAfter = -1;
  /* 「频率轴」联动：控件值会变是理所当然的，**频段划分表的指纹**会不会变
     才是判据 —— 这一项曾经切了完全没反应（表只在 start() 里建过一次）。 */
  bool bandPickWorks = false;
  bool bandSigChanged = false;
  bool bandSigRestored = false;
  long long bandSigBefore = 0, bandSigAfter = 0;

  /* 参数预设：参考实现参数页顶上那套「预设 / 保存参数 / 删除」。
     分四项判据，每项对应一种真实的失效方式：
       · 存了但没进下拉   → 「选了没反应」的空选项
       · 载入后参数没回来 → 覆盖不全 / 漏参数
       · 文件没写出去     → 报告说有、重启就没了
       · 动了不该动的     → 「显示在哪块屏 / 声音来源」被预设带跑 */
  bool presetSaved = false;
  bool presetInOptions = false;
  bool presetVisualBack = false;
  bool presetAudioBack = false;
  bool presetBandBack = false;
  bool presetPickWorks = false;
  bool presetFileOk = false;
  bool presetFileRoundTrip = false;
  bool presetOverwriteOk = false;
  /* ★ 下面三项是 v9 补的，专门兜「预设从磁盘读回来」这条路。
     v8 的自检只测了「存了再套」和「写出去的值对不对」，两条都没经过
     「把文件读进内存」这一步 —— 而那一步正是出事故的地方。 */
  bool presetLoadKeepsSet = false;     // loadPresets() 不许改动 g_set
  bool presetDiskReload = false;       // 丢掉内存档位 → 从文件重载 → 套用，值要回来
  bool presetSelHonest = false;        // 下拉不许说谎：只在参数真的等于某套时才显示它
  bool presetStartupOrderOk = false;   // saveSettings→loadSettings→loadPresets 后值仍在
  bool presetKeepMachine = false;
  bool presetDeleted = false;
  int presetSavedCount = 0;

  /* 帧节奏：把「看着卡」量化。均值帧率看不出偶发长帧，
     所以额外记 p95 / 最长帧 —— 卡顿的观感几乎全由长尾决定。
     ⚠ 自检全程开着交换链回读（每帧 5.76MB 的 GPU→CPU 拷贝 + Map 同步等待），
       那个帧率**不能**代表正常使用。所以结尾另开一段「无回读」窗口单独统计。 */
  double ftSum = 0.0, ftMax = 0.0;
  long ftCount = 0;
  std::vector<double> ftSamples;
  double ftP50 = 0.0, ftP95 = 0.0;
  bool didPerf = false, perfWindow = false;
  double perfUntil = 0.0;
  double ftSum2 = 0.0, ftMax2 = 0.0;
  long ftCount2 = 0;
  std::vector<double> ftSamples2;
  std::vector<double> hitchAt;   // 长帧出现时的累计秒数（诊断用）
  /* 长帧归因：那 400ms 到底花在渲染上，还是花在等 / 别处？ */
  double renderMaxMs = 0.0, renderSumMs = 0.0;
  double slowMapMs = 0.0, slowPresentMs = 0.0;   // 最慢那一帧的分阶段耗时
  /* 画面内容真正变化了多少次（只比音频驱动的那 11 个量，不含 uTime）。
     用来客观回答「帧率上限调了到底有没有用」：
     渲染 144 帧/秒而内容只变 30 次/秒 → 4/5 的帧是重复的，用户看不出区别。 */
  long contentChanges = 0;
  float lastBands[11] = {0};
  bool lastBandsInit = false;
  long renderCount = 0;

  /* 窗口属性 */
  bool wTopmost = false, wToolWindow = false, wNoRedir = false;
  bool wClickThrough = false;      // 窗口过程对 WM_NCHITTEST 的回答（**不是**真实穿透）
  bool wTransparentEx = false;     // WS_EX_TRANSPARENT
  bool wLayered = false;           // ★ WS_EX_LAYERED —— 和 TRANSPARENT 配成一对才穿透
  bool wRealPassThrough = false;   // ★ 系统真实命中测试：WindowFromPoint 不是我们自己
  bool hitCenterSelf = false;
  int realPassPoints = 0;

  /* CPU 占用率：排除「渲染把 CPU 占满导致桌面卡死」这条歧义 */
  double cpuPercent = 0.0;
  /* 空窗口区域实验：区域会不会连 DirectComposition 的画面一起裁掉 */
  bool regionTestDone = false;
  bool regionKeptContent = false;
  long regionChangedPixels = 0;

  /* 跨进程真实点击穿透验证 */
  double tClick = 21.0;
  bool didClick = false;
  int clickControl = -1;   // 对照组：覆盖层隐藏时收到的点击数
  int clickOverlay = -1;   // 实验组：覆盖层显示时收到的点击数
  const char* clickVerdict = "未执行";
  long presentHr = 0;
  int overlayCount = 0;
  int overlayW = 0, overlayH = 0;

  std::string note;
};

static LpSelfTest g_st;

/* 面板联动测试：直接调面板的测试接口，等价于用户拖动 / 选择。
 *
 *  ⚠ 面板已经换成自绘的了（panel.cpp），不再有子窗口控件，
 *    所以这里不能再用 EnumChildWindows / GetDlgItem 那一套。
 *    改成调 lp_panel_test_* —— 它们走的是**和真实鼠标完全一样**的那条代码路径
 *    （applySliderFromX / notifyChanged），所以验证的仍然是「拖动真的改了值」。 */
static void selftestPanel() {
  showPanel();
  if (!g_panel) {
    g_st.note += "面板创建失败; ";
    return;
  }
  g_st.panelControls = lp_panel_item_count();
  g_st.panelSliders = lp_panel_slider_count();

  /* 滑杆联动：把「总电平」拖到 70% 位置，看绑定值有没有真的变。
     用这一项当探针是有意的 —— 它就是用户报「不管怎么调跳动幅度都很小」
     的那个参数（第一版它的上限写成了 0.5，浏览器是 1.0）。 */
  {
    const float before = g_set.level;
    g_st.sliderBefore = (int)lround(before * 100.f);
    g_st.sliderWorks = lp_panel_test_drag(S_LEVEL, 0.7f);
    g_st.sliderAfter = (int)lround(g_set.level * 100.f);
    g_set.level = before;              // 还原，别影响后面的像素统计
    pushAudioParamsLight();
  }

  /* 主题下拉联动：选到「沧蓝」 */
  {
    g_st.themeBefore = g_set.themeIndex;
    g_st.themeComboWorks = lp_panel_test_pick(S_THEME, 2);
    g_st.themeAfter = g_set.themeIndex;
    g_set.themeAuto = false;
    g_set.themeIndex = g_st.themeBefore;
    syncPanelControls();
  }

  /* ★ 「频率轴」联动：切到等比，看**频段划分表**有没有跟着重建。
     用 lp_panel_test_pick 是刻意的 —— 它走的是「写绑定值 → notifyChanged
     → panelNotify → pushAudioParams」这条和真实点击完全一样的路径，
     所以能抓住「面板忘了重建表」这类 bug。
     ⚠ 判据不能是「控件值变了」：那当然会变（绑定就是个 int*），
       而且第一版就是因为只看控件值才没发现这一项是死的。 */
  {
    g_st.bandSigBefore = g_audio.bandSignature();
    const int keep = g_set.bandMode;
    g_st.bandPickWorks = lp_panel_test_pick(S_BAND_MODE, keep == 0 ? 1 : 0);
    g_st.bandSigAfter = g_audio.bandSignature();
    g_st.bandSigChanged = (g_st.bandSigAfter != g_st.bandSigBefore);
    lp_panel_test_pick(S_BAND_MODE, keep);        // 还原（同样走面板路径）
    g_st.bandSigRestored = (g_audio.bandSignature() == g_st.bandSigBefore);
  }

  /* 「恢复默认设置」按钮：先把总电平改掉，再按恢复默认，看它有没有回到 0.15 */
  {
    g_set.level = 0.77f;
    panelNotify(ID_PANEL_RESET, nullptr);
    g_st.resetButtonWorks = (g_set.level < 0.5f);
  }
  /* ★ 参数预设（参考实现参数页最上面那套「预设 / 保存参数 / 删除」）。
     判据刻意**跨两页**取样：beamInset 是视觉页的，level / bandMode 是声音页的 ——
     「一套预设含两页参数」这句承诺必须真的被验证，而不是写在提示里就算。
     每一项判据都对应一种真实的失效方式（见 LpSelfTest 里那段注释）。 */
  {
    const std::wstring pname = L"自检预设";
    const int keepDisplay = g_set.displayIndex;
    const int keepIdle = g_set.audioIdleMode;

    /* ① 造一组好认的值，存成预设 */
    g_set.beamInset = 0.17f;
    g_set.level = 0.62f;
    g_set.bandMode = 1;
    presetSaveAs(pname);
    g_st.presetSaved = (presetIndexOfName(pname) >= 0);
    g_st.presetSavedCount = (int)g_presets.size();

    /* 名字有没有真的进下拉的选项 —— 不进就是「存了但选不到」 */
    if (g_presetSelItem >= 0 && g_presetSelItem < (int)g_panelItems.size()) {
      for (const auto& s : g_panelItems[g_presetSelItem].options) {
        if (s == pname) g_st.presetInOptions = true;
      }
    }

    /* ② 把它们改掉，再**走面板路径**选中那个预设。
          lp_panel_test_pick 和真点完全同一条路径
          （写绑定值 → notifyChanged → panelNotify → presetSelectChanged）。
          ⚠ 先退回「未选择」：presetSaveAs 已经把选择位指到它了，
            指到同一个值不算「变化」，pick 会直接返回 false。 */
    g_set.beamInset = -0.11f;
    g_set.level = 0.21f;
    g_set.bandMode = 0;
    g_presetSel = 0;
    const int idx = presetIndexOfName(pname);
    if (idx >= 0) {
      g_st.presetPickWorks = lp_panel_test_pick(S_PRESET_SEL, idx + 1);
      g_st.presetVisualBack = (fabsf(g_set.beamInset - 0.17f) < 1e-4f);
      g_st.presetAudioBack = (fabsf(g_set.level - 0.62f) < 1e-4f);
      /* 频段模式回来了**而且**频段划分表真的重建了 —— 和「频率轴联动」同一个判据。
         bandMode 是预设的一部分，漏了这一步就会「数字变了、画面没变」。 */
      g_st.presetBandBack = (g_set.bandMode == 1 && g_audio.bandMode == 1);
    }

    /* ③ 预设不许动「显示在哪块屏 / 声音来源」（那是这台机器的选择） */
    g_st.presetKeepMachine = (g_set.displayIndex == keepDisplay &&
                              g_set.audioIdleMode == keepIdle);

    /* ④ 文件真的写出去了？报告里有统计、磁盘上没文件是另一种骗人。
       ⚠ 只查「文件存在」还不够 —— 空文件也存在。所以再**从磁盘读回来一遍**，
         证明写出去的键能被原样读回（loadPresets 走的就是这条读法）。
         这一步也顺带证明了 section / 键名拼写没错位。 */
    g_st.presetFileOk =
        (GetFileAttributesW(presetPath().c_str()) != INVALID_FILE_ATTRIBUTES);
    {
      const std::wstring pp = presetPath();
      wchar_t nm[128] = {};
      wchar_t bi[64] = {};
      GetPrivateProfileStringW(L"preset0", L"name", L"", nm, 127, pp.c_str());
      GetPrivateProfileStringW(L"preset0", L"beamInset", L"", bi, 63, pp.c_str());
      g_st.presetFileRoundTrip =
          (std::wstring(nm) == pname && bi[0] != 0 &&
           fabsf((float)_wtof(bi) - 0.17f) < 1e-4f);
    }

    /* ★⑤ 从磁盘重载（模拟「退出重开程序，再选那个预设」）。
          这一步是 v9 补的，也正是 v8 事故的藏身处。
          ⚠ 为什么前面 ② 抓不到：那条路上档位的值还在**内存里的 kv** 里，
            从头到尾没经过「把文件读进 kv」这一步 —— 而 bug 恰好在那一句。
          做法：先把 g_set 设成**哨兵值**，调 loadPresets()，看它有没有被动过；
          再把内存里的档位整个丢掉、从文件重新读一遍，然后套用。 */
    {
      /* ⑤-1 loadPresets() 只许读磁盘上的档位，一个字都不许动 g_set。
             （v8 在这里把所有设置冲成了出厂默认值。） */
      g_set.level = 0.99f;
      g_set.beamInset = -0.14f;
      loadPresets();
      g_st.presetLoadKeepsSet =
          (fabsf(g_set.level - 0.99f) < 1e-6f && fabsf(g_set.beamInset + 0.14f) < 1e-6f);

      /* ⑤-2 丢掉内存 → 从文件重载 → 走面板路径套用 */
      g_set.level = 0.01f;
      g_set.beamInset = -0.01f;
      g_set.bandMode = 0;
      const int idx2 = presetIndexOfName(pname);
      if (idx2 >= 0) {
        g_presetSel = 0;
        g_st.presetDiskReload = lp_panel_test_pick(S_PRESET_SEL, idx2 + 1);
        g_st.presetDiskReload =
            g_st.presetDiskReload && fabsf(g_set.beamInset - 0.17f) < 1e-4f &&
            fabsf(g_set.level - 0.62f) < 1e-4f && g_set.bandMode == 1;
      }

      /* ⑤-3 下拉不许说谎：套完之后「当前参数 == 这一套」必须成立；
            随手改一个值之后必须**不**成立（否则下拉会一直显示某个预设，
            而参数其实是别的 —— 用户只会得出「预设没生效」这一个结论）。 */
      if (idx2 >= 0) {
        const bool matchAfterApply = presetMatchesCurrent(idx2);
        const float saveLv = g_set.level;
        g_set.level = 0.33f;
        const bool matchAfterEdit = presetMatchesCurrent(idx2);
        g_set.level = saveLv;
        g_st.presetSelHonest = (matchAfterApply && !matchAfterEdit);
      }
    }

    /* ★⑥ 整条启动顺序：saveSettings → loadSettings → loadPresets，值必须还在。
          单测 ⑤-1 只证明「loadPresets 不动 g_set」；这一条还要证明
          「loadSettings 真的能把我写出去的东西读回来」，
          两条合起来才是用户那条「改完 → 重启 → 值还在」的路径。
          ⚠ 自检的 iniPath 也被指到输出目录了，所以不会碰用户真实的设置。 */
    {
      const float keepLevel = g_set.level;
      const int keepStyle = g_set.styleMode;
      g_set.level = 0.83f;
      g_set.styleMode = 1;
      saveSettings();
      g_set.level = 0.01f;
      g_set.styleMode = 0;
      loadSettings();
      const bool afterLoad = (fabsf(g_set.level - 0.83f) < 1e-4f && g_set.styleMode == 1);
      loadPresets();
      g_st.presetStartupOrderOk =
          (afterLoad && fabsf(g_set.level - 0.83f) < 1e-4f && g_set.styleMode == 1);
      g_set.level = keepLevel;
      g_set.styleMode = keepStyle;
      /* ⚠ loadPresets() 会按「当前参数是否等于某套」重算选择位，而上面为了测
         存盘读回故意把 g_set 改成过 0.83/1，于是选择位被清成了「未选择」。
         这里补回来 —— 后面 ⑦⑧ 两个按钮分支要靠它才走得到。
         （生产代码里 loadPresets 只在启动时调一次，不存在这个问题。） */
      g_presetSel = presetIndexOfName(pname) + 1;
    }

    /* ⑦ 「保存参数」按钮的**覆盖**语义：已经选中某个预设时，它应该把当前值
          覆盖进那个预设，而不是新建一个。
          ⚠ 这里直接调 panelNotify（= notifyChanged 干的那一件事），
            走的是和真点按钮**完全一样**的分支；上面 ② 用的是 lp_panel_test_pick，
            两者合起来覆盖了「下拉」和「按钮」两条真路径。 */
    {
      const int n0 = (int)g_presets.size();
      g_set.level = 0.44f;                 // 改一个声音页的值
      panelNotify(S_PRESET_SAVE, nullptr); // 应当覆盖，不是新增
      g_st.presetOverwriteOk = ((int)g_presets.size() == n0);
      if (g_st.presetOverwriteOk) {
        const std::wstring pp = presetPath();
        wchar_t lv[64] = {};
        GetPrivateProfileStringW(L"preset0", L"level", L"", lv, 63, pp.c_str());
        g_st.presetOverwriteOk = (lv[0] != 0 && fabsf((float)_wtof(lv) - 0.44f) < 1e-4f);
      }
    }

    /* ⑧ 删掉它（自检不该在输出目录里留垃圾），并确认下拉里也没了。
          同样走 panelNotify —— 即「删除预设」按钮那条真分支。 */
    panelNotify(S_PRESET_DEL, nullptr);
    g_st.presetDeleted = (presetIndexOfName(pname) < 0);
    if (g_st.presetDeleted && g_presetSelItem >= 0 &&
        g_presetSelItem < (int)g_panelItems.size()) {
      for (const auto& s : g_panelItems[g_presetSelItem].options) {
        if (s == pname) g_st.presetDeleted = false;
      }
    }
  }

  pushAudioParams();
  g_st.panelControls = lp_panel_item_count();
  lp_panel_hide();
}

/* 抓一张「覆盖层真的叠在桌面上」的实拍。
   为什么要主程序自己调 shot.exe：沙箱里后台起的进程会被 shell 一起收走，
   跑不完自检；而且 shot.exe 用的是 GDI+，不想把 GDI+ 链接进主程序。 */
static void selftestScreenshot(const wchar_t* name, bool* okFlag) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir = dir.substr(0, slash);

  /* shot.exe 可能在 exe 旁边，也可能在交付包的「工具（可选）」子目录里 */
  std::wstring shot = dir + L"\\shot.exe";
  if (GetFileAttributesW(shot.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const std::wstring alt = dir + L"\\工具（可选）\\shot.exe";
    if (GetFileAttributesW(alt.c_str()) != INVALID_FILE_ATTRIBUTES) shot = alt;
  }
  const std::wstring out = g_st.outDir + L"\\" + name;
  if (GetFileAttributesW(shot.c_str()) == INVALID_FILE_ATTRIBUTES) {
    g_st.note += "找不到 shot.exe，跳过截图; ";
    return;
  }

  std::wstring cmd = L"\"" + shot + L"\" \"" + out + L"\"";
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                     dir.c_str(), &si, &pi)) {
    /* 等它写完，否则主程序先退出会把它的窗口环境一起收掉 */
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (okFlag) *okFlag = (code == 0);
    if (code != 0) g_st.note += "shot.exe 返回非零，截图可能没成功; ";
  } else {
    g_st.note += "启动 shot.exe 失败; ";
  }
}

static void selftestProbeWindow(LpD3DWindow* w) {
  g_st.overlayCount = (int)g_overlays.size();
  g_st.overlayW = w->w;
  g_st.overlayH = w->h;
  const LONG_PTR ex = GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE);
  g_st.wTopmost = (ex & WS_EX_TOPMOST) != 0;
  g_st.wToolWindow = (ex & WS_EX_TOOLWINDOW) != 0;
  g_st.wNoRedir = (ex & 0x00200000L) != 0;   // WS_EX_NOREDIRECTIONBITMAP
  g_st.wTransparentEx = (ex & WS_EX_TRANSPARENT) != 0;
  g_st.wLayered = (ex & WS_EX_LAYERED) != 0;

  /* 覆盖层对命中测试的回答。注意：穿透**不靠**这个 ——
     实测（src/hitlab.cpp）证明 WM_NCHITTEST 回 HTTRANSPARENT 只对
     **同一线程**的底层窗口生效，跨进程时点击会被直接丢弃。
     这里读一下只是留个记录，真正的判据是下面的 WindowFromPoint
     和最终那个跨进程点击验证。 */
  const LRESULT ht = SendMessageW(w->hwnd, WM_NCHITTEST, 0,
                                  MAKELPARAM(w->w / 2, w->h / 2));
  g_st.wClickThrough = (ht == HTTRANSPARENT);

  /* ★ 真实命中测试：WindowFromPoint 走的是系统给鼠标消息选窗口的同一套逻辑。
     如果它返回的是我们自己，说明点击会被我们吃掉。
     （穿透的正解是 WS_EX_LAYERED | WS_EX_TRANSPARENT 这一对 + 一次
       SetLayeredWindowAttributes —— 见 d3d.cpp 里的实测矩阵。） */
  {
    POINT pts[3];
    pts[0].x = w->rect.left + w->w / 2;      // 屏幕正中
    pts[0].y = w->rect.top + w->h / 2;
    pts[1].x = w->rect.left + 40;            // 左侧光柱附近
    pts[1].y = w->rect.top + w->h - 60;
    pts[2].x = w->rect.left + w->w / 2;      // 底部（任务栏那一条）
    pts[2].y = w->rect.top + w->h - 8;

    int pass = 0;
    for (int i = 0; i < 3; i++) {
      HWND hit = WindowFromPoint(pts[i]);
      /* 命中自己 = 吃掉了点击；命中别的窗口（桌面/任务栏/其他程序）= 真穿透 */
      if (hit != w->hwnd) pass++;
      if (i == 0) g_st.hitCenterSelf = (hit == w->hwnd);
    }
    g_st.wRealPassThrough = (pass == 3);
    g_st.realPassPoints = pass;
  }

  g_st.presentHr = w->lastPresentHr;
}


static void selftestPreRender() {
  /* 保真帧：把音频状态压成探针里的那一套（idle=1、各频段 0），
     这样 maxRgb 才能和 Electron 版实测的 98 直接对比。
     只压「一帧」，压完立刻放开，别影响后面的实时统计。 */
  g_st.forcedFrame = false;
  if (g_st.forceNext) {
    g_st.forcedFrame = true;
    g_audio.idleWave = 1.0f;
    g_audio.bands = LpBands{};
  }
}

/* 返回 true 表示自检结束，该退出了 */
static bool selftestPostRender() {
  const double t = (nowMs() - g_st.tStart) / 1000.0;
  g_st.frames++;

  /* 音频峰值：这一项是「真有声音时光柱会不会跳」的客观证据 */
  const LpBands& bd = g_audio.bands;
  if (g_audio.meterLevel > g_st.peakMeter) g_st.peakMeter = g_audio.meterLevel;
  if (bd.subBass > g_st.peakSubBass) g_st.peakSubBass = bd.subBass;
  if (bd.bass > g_st.peakBass) g_st.peakBass = bd.bass;
  if (bd.mid > g_st.peakMid) g_st.peakMid = bd.mid;
  if (bd.presence > g_st.peakPresence) g_st.peakPresence = bd.presence;
  if (bd.brilliance > g_st.peakBrilliance) g_st.peakBrilliance = bd.brilliance;
  if (bd.air > g_st.peakAir) g_st.peakAir = bd.air;
  if (bd.energy > g_st.peakEnergy) g_st.peakEnergy = bd.energy;
  if (g_st.firstSoundAt < 0.0 && g_audio.meterLevel > 0.02) g_st.firstSoundAt = t;

  if (!g_overlays.empty() && !g_st.perfWindow) {
    for (auto* w : g_overlays) accumulatePixels(w, &g_st.live);
  }

  if (!g_st.didPanel && t >= g_st.tPanel) {
    g_st.didPanel = true;
    selftestPanel();
    if (!g_overlays.empty()) selftestProbeWindow(g_overlays[0]);
  }

  if (!g_st.didShot && t >= g_st.tShot) {
    /* ⚠ 要挑「有声音」的时刻抓。光柱的亮度跟着音频走，在音频谷点抓的话
       画面里两根光柱几乎看不见 —— 看起来就像「效果没生效」。
       实测同一个 exe 抓到的单帧，「最大RGB」能在 28~148 之间跳。
       所以等一个响亮的瞬间；但最多多等 2.5 秒，而且不能越过换主题那一拍
       （否则抓到的会是正在换色的中间态）。 */
    double deadline = g_st.tShot + 2.5;
    if (g_st.tTheme > 0.0 && deadline > g_st.tTheme - 0.2) deadline = g_st.tTheme - 0.2;
    if (deadline < g_st.tShot) deadline = g_st.tShot;
    if (g_audio.meterLevel > 0.12f || t >= deadline) {
      g_st.didShot = true;
      selftestScreenshot(L"桌面合成实拍.png", &g_st.shotLaunched);
    }
  }

  /* 再把面板打开抓一张，证明托盘 / 面板真的能用。
     两个口径都留：① 面板**自己的渲染缓冲**（分层窗口的画面，直接导 BMP，
     不依赖桌面能不能截屏）；② 桌面实拍（证明它真的叠在桌面上）。 */
  if (!g_st.didPanelShot && t >= g_st.tPanelShot) {
    g_st.didPanelShot = true;
    showPanel();
    if (g_panel) SetWindowPos(g_panel, HWND_TOP, 60, 80, 0, 0,
                              SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    for (int i = 0; i < 12; i++) {
      MSG m;
      while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
      }
      Sleep(16);
    }
    std::vector<unsigned char> buf;
    int pw = 0, ph = 0;
    if (lp_panel_capture(&buf, &pw, &ph) && pw > 0 && ph > 0) {
      writeBmp32(g_st.outDir + L"\\调节面板渲染-声音页.bmp", buf.data(), pw, ph);
      g_st.panelCaptureOk = true;
    }
    /* 再切到「参数」页导一张：两页都要有证据 */
    lp_panel_set_tab(1);
    if (lp_panel_capture(&buf, &pw, &ph) && pw > 0 && ph > 0) {
      writeBmp32(g_st.outDir + L"\\调节面板渲染-参数页.bmp", buf.data(), pw, ph);
    }
    lp_panel_set_tab(0);
    selftestScreenshot(L"调节面板实拍.png", &g_st.panelShotLaunched);
    lp_panel_hide();
  }

  /* ---- 区域实验：窗口区域到底管不管 DirectComposition 的画面？ ----
     分两步测，因为「空区域」可能是特例（窗口零面积 → DWM 整个跳过合成）：
       ① 区域 = 屏幕左半  —— 如果右半的光柱还在，说明区域**不**裁 DComp 内容，
          那就可以用「区域只剩一个像素」把命中面积压到几乎为零，且画面完整；
       ② 区域 = 空        —— 作为对照，确认零面积时内容确实会消失。 */
  if (g_st.regionStage == 0 && t >= g_st.tRegion) {
    g_st.regionStage = 1;
    selftestScreenshot(L"区域实验-0-无区域.png", nullptr);
    for (auto* w : g_overlays) {
      HRGN half = CreateRectRgn(0, 0, w->w / 2, w->h);
      SetWindowRgn(w->hwnd, half, TRUE);   // 系统接管 HRGN 所有权
    }
    g_st.tRegionNext = t + 1.2;
  } else if (g_st.regionStage == 1 && t >= g_st.tRegionNext) {
    g_st.regionStage = 2;
    selftestScreenshot(L"区域实验-1-左半区域.png", nullptr);
    for (auto* w : g_overlays) {
      HRGN empty = CreateRectRgn(0, 0, 0, 0);
      SetWindowRgn(w->hwnd, empty, TRUE);
    }
    g_st.tRegionNext = t + 1.2;
  } else if (g_st.regionStage == 2 && t >= g_st.tRegionNext) {
    g_st.regionStage = 3;
    selftestScreenshot(L"区域实验-2-空区域.png", nullptr);
    for (auto* w : g_overlays) SetWindowRgn(w->hwnd, nullptr, TRUE);   // 还原
    g_st.regionTestDone = true;
  }

  /* ---- 跨进程真实点击穿透验证（带对照组）---- */
  if (!g_st.didClick && t >= g_st.tClick && g_st.tEnd >= 20.0) {
    g_st.didClick = true;
    /* ⚠ selftestClickThrough() 是**同步阻塞**的：它要等两个探针进程
       （每个最长 8 秒等 ready 文件 + 最长 20 秒等退出）。这段时间主循环
       一帧都不走，但 t 是墙钟算出来的，会照常往前走 —— 于是排在它后面的
       「无回读性能窗口」会被整个跳过。实测 tEnd=22 时
       「帧节奏_无回读」样本 = 0（12 秒档不跑点击验证，所以只有它好使）。
       办法：把阻塞掉的时间从 tStart 上补回来，等价于「点验证期间暂停秒表」。
       tStart 只被 t 的计算用到，所以这样做是安全的。
       t 会从 tClick 接着走，后面的窗口照常拿到帧，而
       「时长秒」仍然是用户要求的 22 —— 秒表量的就是有效测试时长。 */
    const double before = nowMs();
    selftestClickThrough();
    g_st.tStart += nowMs() - before;
  }

  /* 保真帧：让下一帧带上「强制 idle=1」的状态，采完再放开 */
  if (!g_st.didFidelity && !g_st.forceNext && t >= g_st.tFidelity) {
    g_st.forceNext = true;
  }
  if (g_st.forcedFrame) {
    g_st.didFidelity = true;
    g_st.forceNext = false;
    for (auto* w : g_overlays) accumulatePixels(w, &g_st.fidelity);
    if (!g_overlays.empty() && !g_overlays[0]->lastFrame.empty()) {
      /* 记下写盘结果。这张图是「保真帧」的唯一物证 ——
         报告里有像素统计、磁盘上却没有图，就会让人以为统计是编的。
         之前 v11 那轮就是这样：统计正常、图不见了，查了半天。
         写成功与否必须显式落到报告里，别靠"文件在不在"去猜。 */
      g_st.fidelityBmpOk =
          writeBmp32(g_st.outDir + L"\\覆盖层回读-主题霁紫.bmp",
                     g_overlays[0]->lastFrame.data(), g_overlays[0]->w, g_overlays[0]->h);
    }
  }

  if (!g_st.didTheme && t >= g_st.tTheme) {
    g_st.didTheme = true;
    /* 换到「薄荷」，看像素是不是真的变了。按 key 查表 —— 别用魔数下标，
       主题表顺序变了就会静默换错主题（这个坑刚踩过一次）。 */
    for (int i = 0; i < LP_THEME_COUNT; i++) {
      if (strcmp(LP_THEMES[i].key, "mint-fresh") == 0) {
        g_set.themeIndex = i;
        break;
      }
    }
    if (g_panel) syncPanelControls();
    g_st.themePixelAt = t + 0.5;      // 过渡先走一会儿再开始取样
    /* ⚠ 窗口不能和「窗口区域实验」重叠：那个实验会把覆盖层裁成左半张、
       甚至裁成空区域（整窗不可见）。一旦重叠，采到的就是被裁过的帧
       —— 空区域那一步会直接给出一张全黑的「换色帧」，比音频谷点更误导。
       所以窗口右端要卡在区域实验之前。 */
    g_st.themeWindowEnd = t + 2.0;
    if (g_st.tRegion > 0.0 && g_st.themeWindowEnd > g_st.tRegion - 0.3) {
      g_st.themeWindowEnd = g_st.tRegion - 0.3;
    }
    if (g_st.themeWindowEnd < g_st.themePixelAt + 0.2) {
      g_st.themeWindowEnd = g_st.themePixelAt + 0.2;   // 兜底：至少采 0.2 秒
    }
    g_st.themedBestMaxRgb = -1;
    g_st.themedBest.clear();
  }
  /* 窗口内逐帧比「最亮」，只把最亮的那一帧留档（比每帧都拷一份便宜得多）。 */
  if (g_st.themePixelAt > 0.0 && t >= g_st.themePixelAt && t <= g_st.themeWindowEnd &&
      !g_overlays.empty() && !g_overlays[0]->lastFrame.empty()) {
    const auto* w0 = g_overlays[0];
    const int mx = frameMaxRgb(w0->lastFrame.data(), w0->w, w0->h);
    if (mx > g_st.themedBestMaxRgb) {
      g_st.themedBestMaxRgb = mx;
      g_st.themedBest = w0->lastFrame;
    }
  }
  /* 窗口结束：对留档的那一帧做统计并落盘。 */
  if (g_st.themeWindowEnd > 0.0 && t > g_st.themeWindowEnd &&
      g_st.themed.samples == 0 && !g_st.themedBest.empty() && !g_overlays.empty()) {
    const auto* w0 = g_overlays[0];
    accumulatePixelsBuf(g_st.themedBest.data(), w0->w, w0->h, &g_st.themed);
    writeBmp32(g_st.outDir + L"\\覆盖层回读-换色后.bmp",
               g_st.themedBest.data(), w0->w, w0->h);
  }

  /* ---- 无回读性能窗口 ----
     自检全程开着交换链回读（每帧 5.76MB 的 GPU→CPU 拷贝 + Map 同步等待），
     实测会把帧率压到 45~50。那不是用户实际看到的数字。
     所以结尾留 1.2 秒把回读关掉，单独量一次「正常使用时」的帧节奏。 */
  if (!g_st.didPerf && t >= g_st.tEnd - 2.2) {
    g_st.didPerf = true;
    g_st.perfWindow = true;
    g_st.perfUntil = t + 1.2;
    for (auto* w : g_overlays) w->wantReadback = false;
  }
  if (g_st.perfWindow && t >= g_st.perfUntil) {
    g_st.perfWindow = false;
    for (auto* w : g_overlays) w->wantReadback = true;
  }

  if (t >= g_st.tEnd) {
    g_st.fps = g_st.frames / t;
    return true;
  }
  return false;
}

static void selftestWriteReport() {
  const std::wstring p = g_st.outDir + L"\\自检报告.json";
  FILE* f = _wfopen(p.c_str(), L"wb");
  if (!f) return;
  auto px = [&](const char* name, const LpPixelStats& s, bool last) {
    fprintf(f,
            "    \"%s\": {\"采样\":%lld,\"最大alpha\":%d,\"最大RGB\":%d,\"平均alpha\":%.3f,"
            "\"alpha为零占比\":%.4f,\"亮区bbox\":[%d,%d,%d,%d],\"左右最大色差\":%.0f}%s\n",
            name, s.samples, s.maxAlpha, s.maxRgb, s.meanAlpha(),
            s.samples ? (double)s.zeroAlpha / (double)s.samples : 0.0,
            s.bboxValid() ? s.bl : -1, s.bboxValid() ? s.bt : -1,
            s.bboxValid() ? s.br : -1, s.bboxValid() ? s.bb : -1, s.symMaxDiff,
            last ? "" : ",");
  };
  fprintf(f, "{\n");
  fprintf(f, "  \"程序\": \"光柱 · 原生版\",\n");
  fprintf(f, "  \"时长秒\": %.2f,\n", g_st.tEnd);
  fprintf(f, "  \"渲染帧数\": %ld,\n", g_st.frames);
  fprintf(f, "  \"实测帧率\": %.1f,\n", g_st.fps);
  fprintf(f, "  \"覆盖层\": {\"数量\":%d,\"宽\":%d,\"高\":%d,\"Present_HRESULT\":%ld,"
             "\"置顶\":%s,\"工具窗口\":%s,\"无重定向表面\":%s,"
             "\"WS_EX_TRANSPARENT\":%s,\"WS_EX_LAYERED\":%s,"
             "\"WM_NCHITTEST回HTTRANSPARENT\":%s,"
             "\"系统实测可穿透点数\":%d,\"系统实测鼠标穿透\":%s,\"屏幕正中命中自己\":%s},\n",
          g_st.overlayCount, g_st.overlayW, g_st.overlayH, g_st.presentHr,
          g_st.wTopmost ? "true" : "false", g_st.wToolWindow ? "true" : "false",
          g_st.wNoRedir ? "true" : "false", g_st.wTransparentEx ? "true" : "false",
          g_st.wLayered ? "true" : "false",
          g_st.wClickThrough ? "true" : "false",
          g_st.realPassPoints, g_st.wRealPassThrough ? "true" : "false",
          g_st.hitCenterSelf ? "true" : "false");
  fprintf(f, "  \"托盘\": {\"图标已注册\":%s},\n", g_trayOk ? "true" : "false");
  {
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
      fprintf(f,
              "  \"内存\": {\"工作集MB\":%.1f,\"峰值工作集MB\":%.1f,\"私有提交MB\":%.1f,"
              "\"页面文件MB\":%.1f},\n",
              pmc.WorkingSetSize / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0,
              pmc.PagefileUsage / 1048576.0, pmc.PagefileUsage / 1048576.0);
    }
  }
  {
    /* CPU 占用率 = 进程 CPU 时间 / 墙钟时间。用来排除
       「渲染把 CPU 占满导致桌面点不动」这条歧义。 */
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
      auto toSec = [](const FILETIME& ft) {
        ULARGE_INTEGER v;
        v.LowPart = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        return (double)v.QuadPart / 10000000.0;
      };
      const double cpu = toSec(k) + toSec(u);
      const double wall = g_st.tEnd;
      fprintf(f, "  \"CPU\": {\"CPU秒\":%.2f,\"墙钟秒\":%.2f,\"占用率\":%.1f},\n", cpu, wall,
              wall > 0 ? cpu / wall * 100.0 : 0.0);
    }
  }
  fprintf(f, "  \"面板\": {\"项数\":%d,\"滑杆数\":%d,\"滑杆联动\":%s,\"总电平百分比\":[%d,%d],"
             "\"主题下拉联动\":%s,\"主题索引\":[%d,%d],\"恢复默认可用\":%s,"
             "\"渲染缓冲已导出\":%s},\n",
          g_st.panelControls, g_st.panelSliders, g_st.sliderWorks ? "true" : "false",
          g_st.sliderBefore, g_st.sliderAfter,
          g_st.themeComboWorks ? "true" : "false", g_st.themeBefore, g_st.themeAfter,
          g_st.resetButtonWorks ? "true" : "false",
          g_st.panelCaptureOk ? "true" : "false");
  /* ★ 「频率轴」这一项单独报，而且报的是**频段划分表的指纹**而不是控件值：
     它曾经切了完全没反应（控件值当然会变，表却只在 start() 里建过一次）。 */
  fprintf(f, "  \"频率轴联动\": {\"控件值改变\":%s,\"频段表指纹改变\":%s,"
             "\"指纹_切换前\":%lld,\"指纹_切换后\":%lld,\"切回后指纹还原\":%s},\n",
          g_st.bandPickWorks ? "true" : "false",
          g_st.bandSigChanged ? "true" : "false",
          g_st.bandSigBefore, g_st.bandSigAfter,
          g_st.bandSigRestored ? "true" : "false");
  /* ★ 参数预设（参考实现参数页最上面那套「预设 / 保存参数 / 删除」）。
     报的是**跨两页的取证结果**，不是「控件值变了」—— 那只能证明消息通了：
       · 视觉页的 beamInset、声音页的 level / bandMode 三样都必须回来；
       · bandMode 还额外查了 g_audio.bandMode（引擎侧）而不仅是 g_set（设置结构），
         因为「结构里对了但没推到引擎」正是 v7 踩过的那个坑；
       · 同时查 displayIndex / audioIdleMode **没有**被预设改动 ——
         那两项是「这台机器怎么用」，不是参考实现参数。 */
  fprintf(f, "  \"参数预设\": {\"保存成功\":%s,\"出现在下拉里\":%s,"
             "\"视觉参数载回\":%s,\"音频参数载回\":%s,\"频段表已重建\":%s,"
             "\"下拉选择生效\":%s,\"保存参数覆盖语义\":%s,"
             "\"预设文件已写出\":%s,\"磁盘读回一致\":%s,"
             "\"载入预设不动当前配置\":%s,\"从磁盘重载后套用\":%s,\"下拉不说谎\":%s,"
             "\"启动顺序不冲配置\":%s,"
             "\"不动机器项_显示与来源\":%s,\"删除干净\":%s,\"删除后下拉同步\":%s,"
             "\"存档时预设数\":%d},\n",
          g_st.presetSaved ? "true" : "false",
          g_st.presetInOptions ? "true" : "false",
          g_st.presetVisualBack ? "true" : "false",
          g_st.presetAudioBack ? "true" : "false",
          g_st.presetBandBack ? "true" : "false",
          g_st.presetPickWorks ? "true" : "false",
          g_st.presetOverwriteOk ? "true" : "false",
          g_st.presetFileOk ? "true" : "false",
          g_st.presetFileRoundTrip ? "true" : "false",
          g_st.presetLoadKeepsSet ? "true" : "false",
          g_st.presetDiskReload ? "true" : "false",
          g_st.presetSelHonest ? "true" : "false",
          g_st.presetStartupOrderOk ? "true" : "false",
          g_st.presetKeepMachine ? "true" : "false",
          g_st.presetDeleted ? "true" : "false",
          g_st.presetDeleted ? "true" : "false",
          g_st.presetSavedCount);
  {
    /* 帧节奏：均值帧率掩盖长尾，卡顿的观感几乎全由 p95 / 最长帧决定 */
    auto stats = [](std::vector<double>& v, double* p50, double* p95) {
      *p50 = *p95 = 0.0;
      if (v.empty()) return;
      std::sort(v.begin(), v.end());
      *p50 = v[v.size() / 2];
      const size_t i95 = (size_t)((double)v.size() * 0.95);
      *p95 = v[i95 < v.size() ? i95 : v.size() - 1];
    };
    double p50 = 0.0, p95 = 0.0;
    stats(g_st.ftSamples, &p50, &p95);
    const double meanMs = g_st.ftCount ? g_st.ftSum / (double)g_st.ftCount * 1000.0 : 0.0;
    fprintf(f, "  \"帧节奏_全程含回读\": {\"样本\":%ld,\"均值毫秒\":%.2f,\"p50毫秒\":%.2f,"
               "\"p95毫秒\":%.2f,\"最长毫秒\":%.2f,\"对应均值帧率\":%.1f},\n",
            g_st.ftCount, meanMs, p50 * 1000.0, p95 * 1000.0, g_st.ftMax * 1000.0,
            meanMs > 0.0 ? 1000.0 / meanMs : 0.0);
    double q50 = 0.0, q95 = 0.0;
    stats(g_st.ftSamples2, &q50, &q95);
    const double meanMs2 =
        g_st.ftCount2 ? g_st.ftSum2 / (double)g_st.ftCount2 * 1000.0 : 0.0;
    fprintf(f, "  \"帧节奏_无回读\": {\"样本\":%ld,\"均值毫秒\":%.2f,\"p50毫秒\":%.2f,"
               "\"p95毫秒\":%.2f,\"最长毫秒\":%.2f,\"对应均值帧率\":%.1f},\n",
            g_st.ftCount2, meanMs2, q50 * 1000.0, q95 * 1000.0, g_st.ftMax2 * 1000.0,
            meanMs2 > 0.0 ? 1000.0 / meanMs2 : 0.0);
  }
  fprintf(f, "  \"音频\": {\"环回启动\":%s,\"运行中\":%s,\"电平\":%.3f,\"空闲波浪\":%.3f,"
             "\"错误\":\"%s\"},\n",
          g_audioOk ? "true" : "false", g_audio.running() ? "true" : "false",
          (double)g_audio.meterLevel, (double)g_audio.idleWave,
          g_audioOk ? "" : g_audioErr.c_str());
  fprintf(f, "  \"音频峰值\": {\"电平\":%.3f,\"首次有声时刻秒\":%.2f,"
             "\"subBass\":%.3f,\"bass\":%.3f,\"mid\":%.3f,\"presence\":%.3f,"
             "\"brilliance\":%.3f,\"air\":%.3f,\"energy\":%.3f},\n",
          (double)g_st.peakMeter, g_st.firstSoundAt, (double)g_st.peakSubBass,
          (double)g_st.peakBass, (double)g_st.peakMid, (double)g_st.peakPresence,
          (double)g_st.peakBrilliance, (double)g_st.peakAir, (double)g_st.peakEnergy);
  fprintf(f, "  \"像素\": {\n");
  px("全过程", g_st.live, false);
  px("保真帧_霁紫_idle1", g_st.fidelity, false);
  px("换色帧_薄荷_窗口内最亮", g_st.themed, true);
  fprintf(f, "  },\n");
  fprintf(f, "  \"主题切换改变了像素\": %s,\n",
          (g_st.fidelity.maxRgb != g_st.themed.maxRgb ||
           fabs(g_st.fidelity.meanAlpha() - g_st.themed.meanAlpha()) > 1e-6)
              ? "true" : "false");
  fprintf(f, "  \"桌面合成实拍\": %s,\n", g_st.shotLaunched ? "true" : "false");
  fprintf(f, "  \"保真帧回读已写出\": %s,\n", g_st.fidelityBmpOk ? "true" : "false");
  fprintf(f, "  \"调节面板实拍\": %s,\n", g_st.panelShotLaunched ? "true" : "false");
  fprintf(f, "  \"调节面板渲染缓冲\": %s,\n", g_st.panelCaptureOk ? "true" : "false");
  fprintf(f, "  \"窗口区域实验\": {\"已执行\":%s,"
             "\"看图\":\"区域实验-0-无区域 / 1-左半区域 / 2-空区域 三张 PNG\"},\n",
          g_st.regionTestDone ? "true" : "false");
  fprintf(f, "  \"点击穿透验证\": {\"对照组_覆盖层隐藏_收到点击\":%d,"
             "\"实验组_覆盖层显示_收到点击\":%d,\"判定\":\"%s\"},\n",
          g_st.clickControl, g_st.clickOverlay, g_st.clickVerdict);
  fprintf(f, "  \"备注\": \"%s\"\n", g_st.note.c_str());
  fprintf(f, "}\n");
  fclose(f);
}

/* 跑一次跨进程点击靶，返回它收到的左键按下次数；启动/读取失败返回 -1。
 *
 * 为什么要跨进程：覆盖层是置顶全屏的，它下面全是别的进程（桌面 / 任务栏 /
 * 别的程序）。在自己进程里放测试窗口，即使穿透是坏的，点击也会落到它身上
 * —— 测出来永远是通过。必须跨进程才是真实场景。
 *
 * ★ 顺序很关键（v3 改）：
 *   ① 靶窗口先建，而且是 WS_EX_TOPMOST —— 保证不会被第三方窗口盖住
 *   ② 它写 ready 后，我们**再把覆盖层顶到最上层**（覆盖层必须压在靶上面，
 *      否则测的是"点在没被覆盖的窗口上"，等于白测）
 *   ③ 写 go 放行注入
 *   之前反过来做（覆盖层先就位、再起靶），结果点击经常被某个第三方窗口
 *   截胡，两边都收不到 —— 被误读成"环境不支持注入输入"。 */
static int runClickProbe(const std::wstring& tag, int x, int y, int w, int h,
                         bool overlayVisible) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir = dir.substr(0, slash);

  std::wstring probe = dir + L"\\clickprobe.exe";
  if (GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const std::wstring alt = dir + L"\\工具（可选）\\clickprobe.exe";
    if (GetFileAttributesW(alt.c_str()) != INVALID_FILE_ATTRIBUTES) probe = alt;
  }
  if (GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    g_st.note += "找不到 clickprobe.exe，跳过点击穿透验证; ";
    return -1;
  }

  const std::wstring res = g_st.outDir + L"\\点击靶-" + tag + L".txt";
  const std::wstring ready = g_st.outDir + L"\\点击靶-" + tag + L"-ready.txt";
  const std::wstring go = g_st.outDir + L"\\点击靶-" + tag + L"-go.txt";
  DeleteFileW(res.c_str());
  DeleteFileW(ready.c_str());
  DeleteFileW(go.c_str());

  wchar_t cmd[2048];
  swprintf(cmd, 2048, L"\"%ls\" %d %d %d %d \"%ls\" \"%ls\" \"%ls\"",
           probe.c_str(), x, y, w, h, res.c_str(), ready.c_str(), go.c_str());

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      dir.c_str(), &si, &pi)) {
    g_st.note += "启动 clickprobe.exe 失败; ";
    return -1;
  }
  CloseHandle(pi.hThread);

  /* ① 等靶窗口就位 */
  for (int i = 0; i < 160; i++) {
    if (GetFileAttributesW(ready.c_str()) != INVALID_FILE_ATTRIBUTES) break;
    Sleep(50);
  }

  /* ② 覆盖层顶到最上层，确保它压在靶上面 */
  if (overlayVisible) {
    for (auto* w : g_overlays) {
      ShowWindow(w->hwnd, SW_SHOWNOACTIVATE);
      lp_reassert_topmost(w);
    }
  }
  Sleep(500);

  /* ③ 放行 */
  FILE* gf = _wfopen(go.c_str(), L"wb");
  if (gf) {
    fputs("go", gf);
    fclose(gf);
  }

  WaitForSingleObject(pi.hProcess, 20000);
  CloseHandle(pi.hProcess);

  FILE* f = _wfopen(res.c_str(), L"rb");
  if (!f) return -1;
  char buf[512] = {};
  fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  const char* p = strstr(buf, "downs=");
  return p ? atoi(p + 6) : -1;
}

static void selftestClickThrough() {
  /* 靶子放在屏幕正中：两侧光柱够不到，这里是纯覆盖层区域，
     正是「点击应该穿过去」的地方。 */
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  const int pw = 240, ph = 160;
  const int px = sw / 2 - pw / 2, py = sh / 2 - ph / 2;

  /* ① 对照组：先把覆盖层藏起来，确认靶子和注入机制本身是好的。
        如果对照组都收不到点击，说明这个环境不支持注入输入，本次验证无效
        —— 必须如实报出来，不能把「测不了」当成「通过」。 */
  for (auto* w : g_overlays) ShowWindow(w->hwnd, SW_HIDE);
  Sleep(400);
  g_st.clickControl = runClickProbe(L"对照-覆盖层已隐藏", px, py, pw, ph, false);

  /* ② 实验组：覆盖层正常显示（置顶全屏，正好盖住靶子） */
  g_st.clickOverlay = runClickProbe(L"实验-覆盖层显示中", px, py, pw, ph, true);

  if (g_st.clickControl < 0 || g_st.clickOverlay < 0) {
    g_st.clickVerdict = "无法验证（clickprobe 没能启动或读取结果）";
  } else if (g_st.clickControl == 0) {
    g_st.clickVerdict = "无法验证（本环境把注入的鼠标输入丢弃了，对照组也收不到点击）";
  } else if (g_st.clickOverlay > 0) {
    g_st.clickVerdict = "通过（覆盖层盖着时，点击仍然到达了下层窗口）";
  } else {
    g_st.clickVerdict = "失败（覆盖层把点击吃掉了）";
  }
}

/* ============================ 计时 ============================ */

static double g_qpcFreq = 0.0;
static LARGE_INTEGER g_qpcStart;

static double nowMs() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)(c.QuadPart - g_qpcStart.QuadPart) * 1000.0 / g_qpcFreq;
}

/* ============================ 主循环 ============================ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
  /* ---- 命令行：--selftest <秒> <输出目录> / --hold <秒> / --perf <秒> ----
   *
   *  --perf 是**干净**的帧节奏测量：不建面板、不开回读、不派生子进程，
   *  只跑真实主循环，退出时把帧间隔的 p50/p95/最长打到 stdout。
   *  为什么要单独一个模式：自检里那些诊断动作（截图、点击靶、交换链回读）
   *  本身就会制造几百毫秒的长帧，混在一起根本分不清「卡是渲染卡的」
   *  还是「卡是自检自己卡的」。 */
  double holdSec = 0.0;
  bool perfMode = false;
  /* --panelhold <秒>：启动就把调节面板打开并停住。
     给外部拖动验证用（dragtest.exe 要找一个真实存在、可拖的面板窗口，
     走托盘菜单太绕，而且面板必须是「用户真的在用」的那个实例）。 */
  bool panelHold = false;
  /* --dumpspec <文件> <标签>：把音频链路里**每一帧的 128 个频谱值**落盘。
     这是「原生 vs 参考实现」逐帧比对的**取样器** —— 光靠听和看没法定位
     「幅度偏小 / 部分节拍不响应」这类偏差，必须把同一串真实频谱
     分别喂给两条分析链（src/audiotest.cpp 与 tools/jsref.js）再 diff。 */
  std::wstring dumpSpecPath, dumpSpecLabel;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
      for (int i = 1; i < argc; i++) {
        /* 跑 N 秒后自己退出。给外部脚本做点击穿透测试用（不用外部 kill） */
        if (wcscmp(argv[i], L"--hold") == 0 && i + 1 < argc) {
          holdSec = _wtof(argv[i + 1]);
          continue;
        }
        if (wcscmp(argv[i], L"--dumpspec") == 0 && i + 2 < argc) {
          dumpSpecPath = argv[i + 1];
          /* 标签必须是 ASCII（会写进 CSV 第一列），非 ASCII 一律降级成 spec */
          for (const wchar_t* p = argv[i + 2]; *p; p++) {
            if (*p < 32 || *p > 126) { dumpSpecLabel = L"spec"; break; }
            dumpSpecLabel.push_back(*p);
          }
          if (dumpSpecLabel.empty()) dumpSpecLabel = L"spec";
          continue;
        }
        if (wcscmp(argv[i], L"--perf") == 0 && i + 1 < argc) {
          holdSec = _wtof(argv[i + 1]);
          perfMode = true;
          continue;
        }
        if (wcscmp(argv[i], L"--panelhold") == 0 && i + 1 < argc) {
          holdSec = _wtof(argv[i + 1]);
          panelHold = true;
          continue;
        }
        if (wcscmp(argv[i], L"--nopresent") == 0) {
          g_noPresent = true;
          continue;
        }
        if (wcscmp(argv[i], L"--swap") == 0 && i + 1 < argc) {
          lp_d3d_set_swap_variant((int)_wtof(argv[i + 1]));
          continue;
        }
        if (wcscmp(argv[i], L"--inset") == 0 && i + 1 < argc) {
          g_winInset = (int)_wtof(argv[i + 1]);
          continue;
        }
        if (wcscmp(argv[i], L"--dwmflush") == 0) {
          g_dwmFlushPace = true;
          lp_d3d_set_dwm_flush(true);
          continue;
        }
        if (wcscmp(argv[i], L"--no1ms") == 0) {
          g_no1ms = true;
          continue;
        }
        /* --showcfg：只做「读 settings.ini → 读 presets.ini」，打印每一步之后的
           内存状态，然后退出（不建覆盖层、不写任何文件）。
           存在的理由：v8 出过一次「每次启动把用户设置冲成默认值」的事故，
           而**面板显示的是内存里的值，光看面板完全看不出启动时被哪一步改过**。
           有这个开关，一眼就能定位是 loadSettings 还是 loadPresets 干的。 */
        if (wcscmp(argv[i], L"--showcfg") == 0) {
          g_showCfg = true;
          continue;
        }
        /* --bands：把两种频率轴各自的频段划分打出来然后退出。
           存在的理由：这一项（面板上的「频率轴」）曾经**切了完全没反应**，
           而报告里看不出任何异常 —— 需要一个能直接证明「它到底改了什么」
           的东西。用法：
             光柱.exe --bands [采样率]   （采样率默认 48000）
           ⚠ 和 --perf 一样，结果同时写进 exe 旁边的
             `频率轴-频段表.txt` —— -mwindows 程序的 stdout 在被管道接走时
             拿不到有效句柄，只靠 printf 会一个字都看不到。 */
        if (wcscmp(argv[i], L"--bands") == 0) {
          const double sr = (i + 1 < argc && argv[i + 1][0] != L'-')
                                ? _wtof(argv[i + 1]) : 48000.0;
          std::wstring bp;
          {
            wchar_t exePath[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
              bp.assign(exePath);
              const size_t sl = bp.find_last_of(L"\\/");
              bp = (sl == std::wstring::npos) ? std::wstring() : bp.substr(0, sl + 1);
            }
            bp += L"频率轴-频段表.txt";
          }
          FILE* bf = _wfopen(bp.c_str(), L"wb");
          g_audio.dumpBands(stdout, sr);
          if (bf) {
            g_audio.dumpBands(bf, sr);
            fclose(bf);
          }
          return 0;
        }
        if (wcscmp(argv[i], L"--selftest") == 0 && i + 2 < argc) {
          g_st.active = true;
          g_st.tEnd = _wtof(argv[i + 1]);
          if (g_st.tEnd < 4.0) g_st.tEnd = 4.0;
          g_st.outDir = argv[i + 2];
          /* ★ 一定要转成**绝对路径**。
             自检要派生子进程（shot.exe 截图 / clickprobe.exe 点击靶），
             而 CreateProcess 时子进程的工作目录被设成 exe 所在目录 ——
             相对路径传过去会被解析到 exe 旁边那个不存在的目录，
             表现是「shot.exe 返回非零」「clickprobe 没能启动」，
             但报告里看不出真正原因（第一版就栽在这，白查了一轮）。 */
          {
            wchar_t full[MAX_PATH * 2] = {};
            const DWORD n = GetFullPathNameW(g_st.outDir.c_str(), MAX_PATH * 2, full, nullptr);
            if (n > 0 && n < MAX_PATH * 2) g_st.outDir = full;
          }
          CreateDirectoryW(g_st.outDir.c_str(), nullptr);
          g_selfTestMode = true;
          /* 时间表按时长等比铺开：前 1/8 建面板，3/8 取保真帧，5/8 换主题 */
          /* 时间表按比例铺开（tEnd ≥ 26 时各阶段都够宽裕） */
          g_st.tPanel = g_st.tEnd * 0.06;
          g_st.tFidelity = g_st.tEnd * 0.19;
          g_st.tShot = g_st.tEnd * 0.31;
          g_st.tTheme = g_st.tEnd * 0.46;
          g_st.tPanelShot = g_st.tEnd * 0.58;
          g_st.tRegion = g_st.tEnd * 0.73;
          g_st.tClick = g_st.tEnd * 0.81;
          break;
        }
      }
      LocalFree(argv);
    }
  }

  /* 单实例：重复双击时不要叠出两个覆盖层 */
  HANDLE mutex = CreateMutexW(nullptr, TRUE, LP_MUTEX_NAME);
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"光柱已经在运行了。\n请到系统托盘里操作（左键开面板，右键菜单）。",
                LP_APP_NAME, MB_ICONINFORMATION);
    return 0;
  }

  g_hInst = hInst;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcFreq = (double)f.QuadPart;
  QueryPerformanceCounter(&g_qpcStart);

  /* Sleep 的默认粒度是 15.6ms，不降到 1ms 根本没法稳在 60fps。
     （诊断模式下可以关掉，用来排除它和显卡驱动/DWM 的相互干扰。） */
  if (!g_no1ms) timeBeginPeriod(1);

  /* DPI 感知：MinGW 的头文件不一定声明这个入口，动态取一下。
     DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4 */
  {
    typedef BOOL(WINAPI * PFN)(HANDLE);
    auto fn = (PFN)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                  "SetProcessDpiAwarenessContext");
    if (fn) fn((HANDLE)-4);
    else SetProcessDPIAware();
  }

  INITCOMMONCONTROLSEX icc = {};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&icc);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  /* 自检模式用出厂默认值，且退出时不回写 —— 别把用户自己的设置冲掉 */
  if (!g_st.active) loadSettings();
  if (g_showCfg) dumpSettings("① 读完 settings.ini（loadSettings）");

  /* 预设也要在这里载入（不能等到开面板才载）：托盘、自检、
     以及「退出前落盘」都可能在面板还没建过的情况下用到它。
     自检时把**两个**路径都指到输出目录，绝不碰用户自己的配置文件。
     ⚠ iniPath 也要覆盖：自检要验「存盘 → 读回 → 载预设」整条启动顺序。 */
  if (g_st.active && !g_st.outDir.empty()) {
    g_iniPathOverride = g_st.outDir + L"\\settings.ini";
    g_presetPathOverride = g_st.outDir + L"\\presets.ini";
  }
  /* ★ 这一步**只许读磁盘上的档位**，绝不许碰 g_set。
     v8 在这里犯过错：它把「读预设文件」写成了「把空 kv 套到 g_set 上」，
     于是每次启动都把所有设置冲成出厂默认值（详见 PresetLoadIo 的注释）。 */
  loadPresets();
  if (g_showCfg) dumpSettings("② 读完 presets.ini（loadPresets，这一步不该改动上面的值）");
  /* --showcfg 到这里就够了：不建覆盖层、不碰音频、**不写任何文件**。 */
  if (g_showCfg) {
    printf("[cfg] （--showcfg 到此为止，不建覆盖层、不写任何文件）\n");
    fflush(stdout);
    if (!g_no1ms) timeEndPeriod(1);
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return 0;
  }
  /* 迁移过就立刻回写一次（带上新的 cfgVer），否则每次启动都要重迁一遍 */
  if (g_migrated) {
    g_migrated = false;
    saveSettings();
  }
  pushAudioParams();

  /* 先建托盘消息窗口并注册 Ctrl+Alt+Q。
     抢在覆盖层之前，是为了保证「覆盖层万一吃掉鼠标」时键盘退路已经就位。 */
  if (!createTrayWindow()) {
    MessageBoxW(nullptr, L"创建托盘消息窗口失败，将没有托盘图标与 Ctrl+Alt+Q 退出热键。",
                LP_APP_NAME, MB_ICONWARNING);
  }

  /* 覆盖层 */
  std::string err;
  if (!buildOverlays(&err)) {
    std::wstring we(err.begin(), err.end());
    MessageBoxW(nullptr, (L"创建覆盖层失败：\n" + we).c_str(), LP_APP_NAME, MB_ICONERROR);
    destroyTray();
    return 1;
  }

  /* 托盘图标 */
  if (!createTray()) {
    MessageBoxW(nullptr, L"创建托盘图标失败。", LP_APP_NAME, MB_ICONERROR);
  }

  /* 音频 */
  applyAudioSource();

  /* --dumpspec：开始录频谱。必须在音频启动之后 ——
     否则录到的头几十帧是「还没攒够 FFT 窗口」的静音。 */
  if (!dumpSpecPath.empty()) {
    char label[64] = "spec";
    WideCharToMultiByte(CP_ACP, 0, dumpSpecLabel.c_str(), -1, label, sizeof(label), nullptr, nullptr);
    if (g_audio.startDump(dumpSpecPath.c_str(), label)) {
      fprintf(stdout, "[dumpspec] 正在录频谱 → %ls（标签 %s，30Hz，每帧 128 值）\n",
              dumpSpecPath.c_str(), label);
      fflush(stdout);
    } else {
      fprintf(stderr, "[dumpspec] 打不开输出文件：%ls\n", dumpSpecPath.c_str());
      fflush(stderr);
    }
  }

  /* --panelhold：把调节面板打开并停住，等外部 dragtest.exe 来拖。
     放在音频之后、进主循环之前 —— 面板的绘制依赖 pushAudioParams 已经跑过一次，
     否则刚打开的那一帧是空白的（会干扰截图对比）。
     showPanel() 内部已经做过 syncPanelControls()，这里不用重复同步。 */
  if (panelHold) showPanel();

  /* 自检：打开交换链回读，并记下起点时间 */
  if (g_st.active) {
    g_selfTestMode = true;
    for (auto* w : g_overlays) w->wantReadback = true;
    g_st.tStart = nowMs();
  }

  /* 高精度可等待定时器（限帧用）。Win10 1803 起才有这个 flag，
     拿不到就退化成 Sleep + 短自旋（见主循环里的注释）。 */
  HANDLE frameTimer = CreateWaitableTimerExW(
      nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,
      TIMER_ALL_ACCESS);
  if (!frameTimer) {
    frameTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  }

  MSG msg;
  double last = nowMs();
  int frames = 0;
  double fpsAccum = 0.0;
  DWORD lastTopmost = GetTickCount();
  DWORD lastSave = GetTickCount();
  double holdStart = 0.0;

  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (g_quitting) break;

    /* 面板的节流重绘每帧补一次（拖滑杆时高频消息只标记不重绘）。
       放在消息泵之后、渲染之前，保证面板画面和覆盖层同帧更新。 */
    lp_panel_pump();

    /* --hold <秒>：跑够就自己退出。给外部的点击穿透测试用 ——
       让程序自己退，比外部 kill 干净（不会留下半死的窗口）。 */
    if (holdSec > 0.0) {
      if (holdStart == 0.0) holdStart = nowMs();
      if ((nowMs() - holdStart) / 1000.0 >= holdSec) break;
    }

    /* 限帧。
     *
     *  Present 在合成交换链上不等垂直同步（实测能跑到 139fps），帧率得自己控。
     *
     *  ★ 这里是「看着卡卡的」的真凶，第一版用的是 Sleep + 兜底 Sleep(0)：
     *    剩余时间 ≥1ms 时 Sleep，<1ms 时 `YieldProcessor(); Sleep(0);`。
     *    Sleep(0) 的语义是「让出**剩下的整个时间片**」—— 不是「睡 0 毫秒」。
     *    一旦落到那个分支，这一帧就可能被推迟一整个调度时间片（十几毫秒），
     *    帧间隔于是从 16.7ms 突然跳到 30ms+。均值帧率看着还行，
     *    观感就是规律性的「顿一下」——正是用户说的「卡卡的」。
     *
     *  现在改用**高精度可等待定时器**（Win10 1803+ 的
     *  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION）：按「绝对目标时刻」等，
     *  精度约 0.5ms，且完全不让出时间片。拿不到这个特性时退回
     *  Sleep + 纯自旋（自旋只用在最后 <1ms，不会烧 CPU）。
     *  另外加了帧间隔统计（p50 / p95 / 最长），把「卡顿」变成可量化的数。 */
    const double targetDt = 1.0 / (double)(g_set.fpsLimit > 0 ? g_set.fpsLimit : 60);
    {
      const double now = nowMs();
      const double dt = (now - last) / 1000.0;
      if (!g_dwmFlushPace && dt < targetDt) {
        const double leftMs = (targetDt - dt) * 1000.0;
        if (frameTimer) {
          LARGE_INTEGER due;
          due.QuadPart = -(LONGLONG)(leftMs * 10000.0);   // 相对时间，单位 100ns
          if (SetWaitableTimer(frameTimer, &due, 0, nullptr, nullptr, FALSE)) {
            /* ★ 这里必须用 MsgWaitForMultipleObjects，**不能**用
               WaitForSingleObject。
               拖动面板 / 拖滑杆时鼠标消息是连续不断的，如果在这里死等定时器，
               消息只能攒到下一帧才被处理 —— 面板就会一顿一顿地跳，
               而且队列里积压的消息被一次性排空时还会互相打架
               （见 panel.cpp 里 g_dragOriginScreen 的注释）。
               让等待同时唤醒在「有消息」和「定时器到点」两个条件上，
               拖动就能跟着光标走。处理完消息后回到循环顶部重新算剩余时间。 */
            if (!perfMode && !g_st.active) {
              HANDLE hs[1] = {frameTimer};
              if (MsgWaitForMultipleObjects(1, hs, FALSE, 50, QS_ALLINPUT) ==
                  WAIT_OBJECT_0 + 1) {
                MSG m2;
                while (PeekMessageW(&m2, nullptr, 0, 0, PM_REMOVE)) {
                  if (m2.message == WM_QUIT) { g_quitting = true; break; }
                  TranslateMessage(&m2);
                  DispatchMessageW(&m2);
                }
              }
            } else {
              WaitForSingleObject(frameTimer, 50);
            }
          }
        } else if (leftMs >= 1.2) {
          Sleep((DWORD)leftMs);
        } else {
          /* 最后不到 1ms：纯自旋。时间极短，代价可以忽略；
             关键是**不要**用 Sleep(0)（见上面的注释）。 */
          while ((nowMs() - now) * 1000.0 < leftMs) YieldProcessor();
        }
        /* 面板的节流重绘不在这里补 —— 已经在循环顶部（消息泵之后）
           统一调 lp_panel_pump()，避免两处重复。 */
        if (g_quitting) break;
        continue;
      }
    }
    double dt = 0.0;
    {
      const double now = nowMs();
      dt = (now - last) / 1000.0;
      if ((g_st.active || perfMode) && dt > 0.0 && dt < 0.5) {
        g_st.ftSum += dt;
        g_st.ftCount++;
        if (dt > g_st.ftMax) g_st.ftMax = dt;
        if (g_st.ftSamples.size() < 200000) g_st.ftSamples.push_back(dt);
        if (g_st.perfWindow) {
          g_st.ftSum2 += dt;
          g_st.ftCount2++;
          if (dt > g_st.ftMax2) g_st.ftMax2 = dt;
          if (g_st.ftSamples2.size() < 200000) g_st.ftSamples2.push_back(dt);
        }
        /* 记下长帧出现的时刻：用来区分「开头的一次性开销」和「周期性卡顿」 */
        if (dt * 1000.0 > targetDt * 1000.0 * 1.5 && g_st.hitchAt.size() < 400)
          g_st.hitchAt.push_back(g_time);
      }
      last = now;
    }
    if (dt > 0.25) dt = 0.25;
    g_time += dt;

    /* 主题自动轮询 */
    if (g_set.themeAuto) {
      g_themeClock += (float)dt;
      if (g_themeClock >= g_set.themeCycleSec) {
        g_themeClock = 0.0f;
        g_set.themeIndex = (g_set.themeIndex + 1) % LP_THEME_COUNT;
        if (g_panel) syncPanelControls();
      }
    }

    /* 音频推进 */
    g_audio.tick(dt);

    /* 自检：渲染前按时间表调整状态 */
    if (g_st.active) selftestPreRender();

    /* 渲染 */
    {
      const double r0 = nowMs();
      for (auto* w : g_overlays) {
        LpParams p;
        fillParams(w, &p);
        /* 统计「画面内容真正变了多少次」。这是「帧率上限调了没用」的判据：
           如果渲染 144 帧/秒而内容只变 30 次/秒，那 4/5 的帧和上一帧
           逐像素相同，用户当然看不出任何区别。
           ⚠ 只能比音频驱动的那 11 个量 —— P5.y（uTime）每帧都在变，
             把它算进去就永远是「变了」。 */
        if (g_st.active || perfMode) {
          const float cur[11] = {p.P1[2], p.P2[0], p.P2[1], p.P2[2], p.P2[3],
                                 p.P3[0], p.P3[1], p.P3[2], p.P3[3],
                                 p.P4[0], p.P4[1]};
          if (g_st.lastBandsInit) {
            bool changed = false;
            for (int i = 0; i < 11; i++)
              if (fabsf(cur[i] - g_st.lastBands[i]) > 1e-4f) { changed = true; break; }
            if (changed) g_st.contentChanges++;
          }
          for (int i = 0; i < 11; i++) g_st.lastBands[i] = cur[i];
          g_st.lastBandsInit = true;
        }
        if (!lp_d3d_render(w, p)) {
          /* 设备丢失：重建一次 */
          std::string e;
          buildOverlays(&e);
          break;
        }
      }
      if (perfMode || g_st.active) {
        const double rm = nowMs() - r0;
        g_st.renderSumMs += rm;
        g_st.renderCount++;
        if (rm > g_st.renderMaxMs) {
          g_st.renderMaxMs = rm;
          /* 记下最慢那一帧各阶段的耗时，直接指出阻塞点 */
          if (!g_overlays.empty()) {
            g_st.slowMapMs = g_overlays[0]->lastMapMs;
            g_st.slowPresentMs = g_overlays[0]->lastPresentMs;
          }
        }
      }
    }

    /* 自检：渲染后收数据 */
    if (g_st.active && selftestPostRender()) break;

    /* 面板电平表 + 状态行 */
    frames++;
    fpsAccum += dt;
    if (fpsAccum >= 0.5) {
      /* 用 0.5 秒窗口的均值，比瞬时 1/dt 稳得多 */
      const double fpsNow = frames / fpsAccum;
      frames = 0;
      fpsAccum = 0.0;
      lp_panel_set_meter(g_audio.meterLevel);
      wchar_t buf[320];
      if (!g_audioOk) {
        /* %hs 在 MinGW 的 swprintf 上不可靠，手动转宽字符 */
        wchar_t werr[192] = {};
        MultiByteToWideChar(CP_UTF8, 0, g_audioErr.c_str(), -1, werr, 192);
        swprintf(buf, 320, L"音频未运行 · %s", werr);
      } else {
        swprintf(buf, 320, L"环回运行中　电平 %.0f%%　空闲波浪 %.2f　渲染 %.0f 次/秒",
                 g_audio.meterLevel * 100.0f, g_audio.idleWave, fpsNow);
      }
      lp_panel_set_status(buf);
    }

    /* 每 2 秒把覆盖层重新顶到最上层：有些全屏应用会抢 Z 序 */
    const DWORD tick = GetTickCount();
    if (tick - lastTopmost > 2000) {
      lastTopmost = tick;
      for (auto* w : g_overlays) lp_reassert_topmost(w);
    }

    /* 配置落盘：面板回调只打脏标记（拖动滑杆时每秒会回调上百次，
       逐次写 INI 会卡手），这里每秒统一落一次。 */
    if (g_settingsDirty && tick - lastSave > 1000) {
      lastSave = tick;
      g_settingsDirty = false;
      saveSettings();
    }
  }

done:
  /* 先把频谱 dump 收尾（fclose 会把缓冲刷出去），再打报告。
     顺序反了的话报告里的数字会比 dump 里的多几帧，对不上。 */
  if (!dumpSpecPath.empty()) {
    g_audio.stopDump();
    fprintf(stdout, "[dumpspec] 已收尾 → %ls\n", dumpSpecPath.c_str());
    fflush(stdout);
  }
  if (perfMode) {
    std::vector<double>& v = g_st.ftSamples;
    std::sort(v.begin(), v.end());
    auto q = [&](double p) {
      if (v.empty()) return 0.0;
      size_t i = (size_t)((double)v.size() * p);
      if (i >= v.size()) i = v.size() - 1;
      return v[i] * 1000.0;
    };
    const double meanMs =
        g_st.ftCount ? g_st.ftSum / (double)g_st.ftCount * 1000.0 : 0.0;
    /* 超过 1.5 倍目标帧时间的帧数：这就是「看着卡」的直接来源 */
    const double targetMs = 1000.0 / (double)(g_set.fpsLimit > 0 ? g_set.fpsLimit : 60);
    long hitches = 0;
    for (double s : v)
      if (s * 1000.0 > targetMs * 1.5) hitches++;
    /* -mwindows 的 GUI 程序不保证有 stdout（从某些宿主启动时拿不到控制台），
       所以同时写一份文件，保证结果一定拿得到。
       ⚠ 两个坑，都踩过：
       ① 文件名里有中文，**必须用 _wfopen**。窄的 fopen 会把这串 UTF-8 字节
          按 ANSI（本机 cp936）解释，实测直接失败返回 NULL —— 于是
          「同时写一份文件保证结果一定拿得到」这句承诺是假的：
          stdout 收不到的时候，结果真就什么都拿不到。
       ② 写到 **exe 旁边**，不写当前工作目录。从快捷方式或别的目录启动时
          CWD 可能是 C:\Windows\System32，要么没权限、要么用户找不到。
          万一 exe 目录不可写，再退回 CWD。 */
    std::wstring perfPath;
    {
      wchar_t exePath[MAX_PATH] = {};
      if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        perfPath.assign(exePath);
        const size_t sl = perfPath.find_last_of(L"\\/");
        perfPath = (sl == std::wstring::npos) ? std::wstring() : perfPath.substr(0, sl + 1);
      }
      perfPath += L"perf-帧节奏.txt";
    }
    FILE* pf = _wfopen(perfPath.c_str(), L"wb");
    if (!pf) pf = _wfopen(L"perf-帧节奏.txt", L"wb");
    auto emit = [&](const char* fmt, ...) {
      va_list a1, a2;
      va_start(a1, fmt);
      va_copy(a2, a1);
      vprintf(fmt, a1);
      if (pf) vfprintf(pf, fmt, a2);
      va_end(a2);
      va_end(a1);
    };
    emit("[perf] 帧率上限 %d fps（目标 %.2f ms）　分析频率 %d Hz　频谱固定 30Hz\n",
         g_set.fpsLimit, targetMs, g_set.anaHz);
    emit("[perf] 样本 %ld　均值 %.2f ms（%.1f fps）　p50 %.2f　p95 %.2f　p99 %.2f　最长 %.2f\n",
         g_st.ftCount, meanMs, meanMs > 0 ? 1000.0 / meanMs : 0.0, q(0.50), q(0.95),
         q(0.99), g_st.ftMax * 1000.0);
    emit("[perf] 长帧（> %.1f ms）%ld 个 / %ld（%.2f%%）\n", targetMs * 1.5, hitches,
         g_st.ftCount, g_st.ftCount ? 100.0 * hitches / (double)g_st.ftCount : 0.0);
    if (!g_st.hitchAt.empty()) {
      emit("[perf] 长帧出现时刻（秒）:");
      for (size_t i = 0; i < g_st.hitchAt.size(); i++) emit(" %.2f", g_st.hitchAt[i]);
      emit("\n");
    }
    emit("[perf] 渲染耗时 均值 %.3f ms　最长 %.3f ms（%ld 帧）"
         "　[最慢帧里] Map+绘制 %.3f ms　Present %.3f ms\n",
         g_st.renderCount ? g_st.renderSumMs / (double)g_st.renderCount : 0.0,
         g_st.renderMaxMs, g_st.renderCount, g_st.slowMapMs, g_st.slowPresentMs);
    /* 主动丢帧数：Present 带 DO_NOT_WAIT 时，后台缓冲没还回来就丢这一帧。
       这个数字应该很小（< 1%），大起来说明 GPU/DWM 压力过大。 */
    long skipped = 0;
    for (auto* w : g_overlays) skipped += w->presentSkipped;
    emit("[perf] 主动丢帧 %ld 个（%.2f%%）　Present 策略=%s\n", skipped,
         g_st.ftCount ? 100.0 * skipped / (double)g_st.ftCount : 0.0,
         (!g_overlays.empty() && g_overlays[0]->presentDoNotWait) ? "DO_NOT_WAIT" : "普通");
    /* ★ 内容变化率：这才是「帧率上限有没有用」的判据。
       三层各有自己的时钟：频谱 30Hz（数据）、分析 anaHz（包络）、
       渲染 fpsLimit（画面）。喂给着色器的值由**分析层**决定，
       所以渲染帧率越高，画面能跟上的变化越多 ——
       变化率应随 fpsLimit 上升（但不会超过分析频率）。 */
    {
      const double secs = g_st.ftSum;
      emit("[perf] 画面内容变化 %ld 次 / %.1f 秒 = %.1f 次/秒"
           "（渲染 %d fps / 分析 %d Hz / 频谱固定 30Hz）\n",
           g_st.contentChanges, secs,
           secs > 0.0 ? (double)g_st.contentChanges / secs : 0.0,
           g_set.fpsLimit, g_set.anaHz);
    }
    /* Present 返回值：OCCLUDED 是「窗口被判为被遮住」，会让 Present 一直阻塞 */
    long occ = 0, oth = 0, bad = 0;
    for (auto* w : g_overlays) { occ += w->presentOccluded; oth += w->presentHrOther; bad = w->presentHrLastBad; }
    emit("[perf] Present 返回：OCCLUDED %ld 次　其它非 S_OK %ld 次　最后一个异常值=0x%08lX\n",
         occ, oth, (unsigned long)bad);
    if (pf) fclose(pf);
    fflush(stdout);
  }
  if (g_st.active) selftestWriteReport();
  else if (holdSec <= 0.0 && g_settingsDirty) saveSettings();
  if (frameTimer) CloseHandle(frameTimer);
  g_audio.stop();
  destroyOverlays();
  destroyTray();
  destroyPanel();
  if (!g_no1ms) timeEndPeriod(1);
  CoUninitialize();
  if (mutex) CloseHandle(mutex);
  return 0;
}
