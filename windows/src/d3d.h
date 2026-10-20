/*
 * 覆盖层窗口 + D3D11 + DirectComposition。
 *
 * 为什么不是 OpenGL：这台机器**没有硬件 OpenGL 驱动**（诊断实测拿到的是
 * 微软 GDI Generic，GL 1.1，wglGetProcAddress 全返回 null），GLSL 着色器根本
 * 编译不了。D3D11 是好的 —— Electron 的 WebGL 本来就是 ANGLE→D3D11。
 *
 * 透明怎么做：这是 Windows 上「带逐像素 alpha 的 GPU 窗口」的正解，
 * 也是 Chromium 自己在用的组合：
 *
 *   ① 窗口加 WS_EX_NOREDIRECTIONBITMAP —— 不走 GDI 重定向表面，
 *      内容完全交给 DirectComposition。
 *   ② 交换链用 CreateSwapChainForComposition + DXGI_ALPHA_MODE_PREMULTIPLIED
 *      —— 显式声明「我给的像素是预乘 alpha 的」，不留任何解释余地。
 *   ③ DirectComposition 把交换链挂到窗口上（visual → target → commit）。
 *   ④ 鼠标穿透用 WM_NCHITTEST 返回 HTTRANSPARENT（文档化、行为稳定）。
 *
 * 合成语义（由 ② 显式保证）：结果 = RGB + 桌面 × (1 - A)。
 * 「纯加光」= RGB 给满亮度、A 给接近 0。
 */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <string>
#include <vector>

struct LpD3DWindow {
  HWND hwnd = nullptr;
  RECT rect = {0, 0, 0, 0};
  int w = 0, h = 0;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain1* swap = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;

  IDCompositionDevice* dcomp = nullptr;
  IDCompositionTarget* target = nullptr;
  IDCompositionVisual* visual = nullptr;

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11RasterizerState* rast = nullptr;

  /* 诊断用：最近一次 Present 的 HRESULT。如果它一直不是 S_OK，
     说明交换链没被真正显示（比如 DComp 没接上）。 */
  long lastPresentHr = 0;

  /* 诊断用：打开后每帧在 Present 之前把后台缓冲回读到 lastFrame。
     用来客观回答「着色器到底产出了什么」，不依赖抓屏（抓屏可能抓不到
     DirectComposition 的内容，也看不出 0.004 这种极低的 alpha）。 */
  bool wantReadback = false;
  std::vector<unsigned char> lastFrame;  // BGRA
  ID3D11Texture2D* staging = nullptr;

  /* 诊断用：上一帧各阶段耗时（毫秒）。定位「偶发 400ms 长帧」到底是
     常量缓冲 Map / 绘制 / Present 哪一步在阻塞 —— 只量总时长是不够的。 */
  double lastMapMs = 0.0, lastDrawMs = 0.0, lastPresentMs = 0.0;

  /* Present 策略：默认带 DXGI_PRESENT_DO_NOT_WAIT（拿不到后台缓冲就丢帧、
     绝不阻塞）。万一这个交换链不支持该标志，运行时会自动退回普通 Present。 */
  bool presentDoNotWait = true;
  long presentSkipped = 0;   // 因「缓冲还没还回来」而主动丢掉的帧数

  /* 纯诊断：完全跳过 Present。用来区分「Present 自己阻塞」和
     「线程在 Present 那几行被系统抢占了」—— 两者的计时表现一模一样。 */
  bool skipPresent = false;

  /* 诊断：Present 返回值统计。
     如果大量出现 DXGI_STATUS_OCCLUDED（窗口被判为「被遮住」），
     Present 就会一直阻塞到窗口重新可见 —— 正好能解释
     「每几秒一次、几百毫秒」的规律性卡顿。 */
  long presentOccluded = 0;
  long presentHrOther = 0;   // 既不是 S_OK 也不是 OCCLUDED 的返回值个数
  long presentHrLastBad = 0; // 最后一个异常返回值，原样打印

  bool valid = false;
};

/* 常量缓冲，布局必须与 shader_src.h 里的 cbuffer LpParams 逐字节一致。
   全用 float4 是刻意的：HLSL 的 cbuffer 不允许变量跨 16 字节边界，
   裸 float/float2 混排时 HLSL 会插空、C++ 不会，这是最典型的静默错位。 */
struct LpParams {
  float P0[4];   // xy = uRes, z = styleMode, w = beamInset
  float P1[4];   // x = audioIntensity, y = responseRange, z = idleWave, w = alphaK
  float P2[4];   // x = subBass, y = bass, z = lowMid, w = mid
  float P3[4];   // x = highMid, y = energy, z = presence, w = brilliance
  float P4[4];   // x = air, y = brightness, z = glowIntensity, w = peakEnabled
  float P5[4];   // x = peakIntensity, y = time
  float cool[4];
  float warm[4];
  float peak[4];
};

/* 建一个透明、置顶、穿透、不进任务栏、不进 alt-tab 的 D3D 窗口。 */
bool lp_d3d_create(HINSTANCE hInst, const RECT& rect, LpD3DWindow* out, std::string* err);

/* 编译并挂上着色器。可以重复调用（换主题不用重建窗口）。 */
bool lp_d3d_set_shaders(LpD3DWindow* w, const char* hlslVs, const char* hlslFs,
                        std::string* err);

/* 更新常量缓冲 + 画一帧 + 垂直同步提交。 */
bool lp_d3d_render(LpD3DWindow* w, const LpParams& p);

void lp_d3d_destroy(LpD3DWindow* w);

void lp_reassert_topmost(LpD3DWindow* w);

/* 诊断用：切换交换链配置变体（0=默认 1=BufferCount16 2=帧延迟等待对象 3=FLIP_DISCARD）。
   必须在 lp_d3d_create 之前调用。 */
void lp_d3d_set_swap_variant(int v);

/* 诊断用：Present 之后调 DwmFlush()，把提交节奏和 DWM 合成节拍对齐。 */
void lp_d3d_set_dwm_flush(bool on);

/* 显示器枚举。rect 用 rcMonitor（整块物理屏），**不是** rcWork —— 不避任务栏。 */
struct LpMonitor {
  RECT rect;
  bool primary;
};
void lp_enum_monitors(std::vector<LpMonitor>* out);
