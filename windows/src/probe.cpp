/*
 * D3D11 透明度探针（P1）。
 *
 * 验证两件事：
 *   1) 这台机器上 D3D11 + DirectComposition 的透明覆盖层能不能建起来
 *   2) 预乘 alpha 的合成语义是不是我们要的「加光」
 *
 * 画三竖条做对照（全屏高）：
 *   左 1/3  RGB = c,     A = 0.004   ← 加光。预乘 → 明显的紫色光晕盖在桌面上；
 *                                        直通 → 几乎看不出变化
 *   中 1/3  RGB = c*0.5, A = 0.5     ← 半透明，两种解释都该「紫 + 桌面压暗一半」
 *   右 1/3  RGB = c,     A = 1.0     ← 实心，两种解释都该是纯紫
 * 左上角一个不透明白方块，用来确认覆盖范围和 uv 方向（该在屏幕左上角）。
 *
 * 用法：probe.exe [秒数]，默认 15 秒后自动退出。
 */
#include "d3d.h"
#include "shader_src.h"

#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static const char* TEST_PS = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(VSOut i) : SV_Target {
  float3 c = float3(0.75, 0.35, 1.0);
  float4 outv;
  if (i.uv.x < 1.0/3.0) {
    outv = float4(c, 0.004);          // 左：加光测试
  } else if (i.uv.x < 2.0/3.0) {
    outv = float4(c * 0.5, 0.5);      // 中：半透明
  } else {
    outv = float4(c, 1.0);            // 右：实心
  }
  // uv.y=1 在上边 → 左上角标记块
  if (i.uv.x < 0.05 && i.uv.y > 0.95) outv = float4(1.0, 1.0, 1.0, 1.0);
  return outv;
}
)HLSL";

/* 回读帧的像素统计（BGRA）。用来客观回答「着色器产出了什么」。 */
static void printFrameStats(const LpD3DWindow& win, const char* label) {
  const std::vector<unsigned char>& f = win.lastFrame;
  if (f.empty()) {
    printf("[probe] %s: 回读为空\n", label);
    return;
  }
  const int W = win.w, H = win.h;
  unsigned maxA = 0, maxRgb = 0;
  long long sumA = 0, sumRgb = 0;
  long long zeroA = 0;
  int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const unsigned char* px = &f[((size_t)y * W + x) * 4];
      const unsigned b = px[0], g = px[1], r = px[2], a = px[3];
      const unsigned m = r > g ? (r > b ? r : b) : (g > b ? g : b);
      if (a > maxA) maxA = a;
      if (m > maxRgb) maxRgb = m;
      sumA += a;
      sumRgb += m;
      if (a == 0) zeroA++;
      if (m > 8) {
        if (x < bx0) bx0 = x;
        if (y < by0) by0 = y;
        if (x > bx1) bx1 = x;
        if (y > by1) by1 = y;
      }
    }
  }
  const long long n = (long long)W * H;
  /* 平均RGB 和平均alpha 一起看：加光式合成里「平均alpha 很低但平均RGB 不低」
     说明画的是稀疏但亮的像素；两个都低才是真的什么都没画。 */
  printf("[probe] %s: 最大alpha=%u 最大RGB=%u 平均alpha=%.2f 平均RGB=%.2f "
         "alpha=0占比=%.2f%% 亮区bbox=(%d,%d)-(%d,%d)\n",
         label, maxA, maxRgb, (double)sumA / n, (double)sumRgb / n,
         100.0 * zeroA / n, bx0, by0, bx1, by1);
  /* 采样几个关键位置：左边缘 / 中左 / 中央 / 右边缘，各取底部区域 */
  const int ys[] = {H - 10, H * 3 / 4, H / 2, H / 4, 10};
  for (int i = 0; i < 5; i++) {
    const int y = ys[i];
    const int xs[] = {2, W / 8, W / 2, W * 7 / 8, W - 3};
    printf("[probe]   y=%4d: ", y);
    for (int j = 0; j < 5; j++) {
      const unsigned char* px = &f[((size_t)y * W + xs[j]) * 4];
      printf("x%4d=(%3u,%3u,%3u,a%3u) ", xs[j], px[2], px[1], px[0], px[3]);
    }
    printf("\n");
  }
  fflush(stdout);
}

int main(int argc, char** argv) {
  int seconds = 15;
  if (argc > 1) seconds = atoi(argv[1]);
  if (seconds <= 0) seconds = 15;
  /* real = 跑真正的光柱着色器（静态参数、无音频），用来目视验证 HLSL 移植 */
  const bool useReal = (argc > 2 && strcmp(argv[2], "real") == 0);

  printf("=== D3D11 透明覆盖层探针 ===\n");

  std::vector<LpMonitor> mons;
  lp_enum_monitors(&mons);
  if (mons.empty()) {
    printf("[probe] 没有枚举到显示器\n");
    return 1;
  }
  const RECT r = mons[0].rect;
  printf("[probe] 主显示器 %ldx%ld @ (%ld,%ld)\n", r.right - r.left, r.bottom - r.top,
         r.left, r.top);

  LpD3DWindow win;
  std::string err;
  if (!lp_d3d_create(GetModuleHandleW(nullptr), r, &win, &err)) {
    printf("[probe] 建窗口失败：%s\n", err.c_str());
    return 1;
  }
  printf("[probe] D3D11 + DirectComposition 窗口建立成功 %dx%d\n", win.w, win.h);

  /* ---- 窗口状态诊断 ----
     上一次探针的现象是「全部成功、138fps、但屏幕上什么都没有」。
     138fps 说明 Present 根本没在等垂直同步，也就是画面没被真正显示。
     所以这里把窗口的可见性 / 位置 / 是否被 DWM 隐藏都打出来。 */
  {
    RECT wr = {};
    GetWindowRect(win.hwnd, &wr);
    printf("[probe] 窗口: visible=%d rect=(%ld,%ld)-(%ld,%ld)\n",
           IsWindowVisible(win.hwnd) ? 1 : 0, wr.left, wr.top, wr.right, wr.bottom);
    DWORD cloaked = 0;
    DwmGetWindowAttribute(win.hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    printf("[probe] DWMWA_CLOAKED = %lu（0 表示没被 DWM 隐藏）\n", cloaked);
    LONG ex = GetWindowLongW(win.hwnd, GWL_EXSTYLE);
    printf("[probe] GWL_EXSTYLE = 0x%08lX（含 NOREDIRECTIONBITMAP=0x200000）\n",
           (unsigned long)ex);
  }

  if (!lp_d3d_set_shaders(&win, LP_HLSL_VS, TEST_PS, &err)) {
    printf("[probe] 着色器失败：%s\n", err.c_str());
    return 1;
  }
  printf("[probe] HLSL 编译成功\n");

  /* 真实光柱着色器也编一遍，提前暴露 HLSL 转换里的语法/语义问题 */
  {
    std::string err2;
    if (!lp_d3d_set_shaders(&win, LP_HLSL_VS, LP_HLSL_FS, &err2)) {
      printf("[probe] ⚠ 光柱着色器编译失败：%s\n", err2.c_str());
      return 1;
    }
    printf("[probe] 光柱着色器编译成功\n");
    if (!useReal) lp_d3d_set_shaders(&win, LP_HLSL_VS, TEST_PS, &err2);  // 换回测试图案
  }

  /* 真实模式的参数：默认主题「霁紫」、风格①跳动、无音频、空闲波浪拉满，
     这样即使没声音也应该看到两根正在呼吸的光柱。 */
  LpParams p = {};
  if (useReal) {
    win.wantReadback = true;
    const float cool[3] = {0.35f, 0.1f, 0.9f};
    const float warm[3] = {0.65f, 0.25f, 1.0f};
    const float peak[3] = {1.0f, 0.55f, 0.05f};
    p.P0[0] = (float)win.w;
    p.P0[1] = (float)win.h;
    p.P0[2] = 0.0f;          // styleMode = jump
    p.P0[3] = 0.0f;          // beamInset
    p.P1[0] = 1.0f;          // audioIntensity
    p.P1[1] = 1.0f;          // responseRange
    p.P1[2] = 1.0f;          // idleWave（拉满，安静时也看得见）
    p.P1[3] = 0.10f;         // alphaK = 纯加光附近
    p.P4[2] = 1.0f;          // glowIntensity
    p.P4[3] = 1.0f;          // peakEnabled
    p.P5[0] = 1.0f;          // peakIntensity
    memcpy(p.cool, cool, sizeof(cool));
    memcpy(p.warm, warm, sizeof(warm));
    memcpy(p.peak, peak, sizeof(peak));
    printf("[probe] 真实模式：主题=霁紫 风格=跳动 idleWave=1 alphaK=0.10\n");
  }

  printf("[probe] 渲染 %d 秒：%s\n", seconds,
         useReal ? "真实光柱（应看到两根呼吸的光柱）"
                 : "左1/3 = 加光(A=0.004)，中1/3 = 半透明，右1/3 = 实心");
  fflush(stdout);

  const DWORD t0 = GetTickCount();
  DWORD lastTop = t0;
  int frames = 0;
  MSG msg;
  while ((GetTickCount() - t0) < (DWORD)seconds * 1000) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (useReal) p.P5[1] = (float)((GetTickCount() - t0) / 1000.0);  // uTime
    if (!lp_d3d_render(&win, p)) {
      printf("[probe] 渲染失败（设备丢失？）\n");
      break;
    }
    frames++;
    if (frames == 30) {
      printf("[probe] 第 30 帧 Present HRESULT = 0x%08lX（S_OK=0）\n",
             (unsigned long)win.lastPresentHr);
      fflush(stdout);
    }
    if (frames == 40 && useReal) printFrameStats(win, "第40帧回读");
    const DWORD now = GetTickCount();
    if (now - lastTop > 2000) {
      lp_reassert_topmost(&win);
      lastTop = now;
    }
  }

  const double secs = (GetTickCount() - t0) / 1000.0;
  printf("[probe] %d 帧 / %.1f 秒 = %.1f fps\n", frames, secs, frames / secs);

  lp_d3d_destroy(&win);
  printf("[probe] 结束\n");
  return 0;
}
