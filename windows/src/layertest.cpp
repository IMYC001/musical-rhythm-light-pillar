/*
 * layertest.exe —— 验证「分层窗口里 alpha=0 的像素，RGB 还显不显示」。
 *
 * 为什么这个实验是决定性的：
 *   实测已确认，唯一能跨进程鼠标穿透的机制是「分层窗口 + 该处 alpha=0」
 *   （WS_EX_TRANSPARENT 和 HTTRANSPARENT 都没用）。但我们的光柱是**加光**：
 *   合成语义是 `结果 = RGB + 桌面 × (1 - A)`，所以 A=0 时 RGB 照样能叠上去。
 *   如果 UpdateLayeredWindow 真的按预乘语义工作，那么
 *   「全屏 alpha=0、RGB=光色」就能同时做到：
 *       · 画面完全正常（纯加光，和现在一模一样）
 *       · 整块屏幕鼠标穿透（因为 alpha 全 0）
 *   那就一举解决所有问题，而且比 DComp 方案更简单。
 *
 * 用法：layertest.exe <alpha 0..255> <rgb 0..255> <秒>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 4) {
    printf("用法: layertest.exe <alpha> <rgb> <秒>\n");
    return 1;
  }
  const int alpha = _wtoi(argv[1]);
  const int rgb = _wtoi(argv[2]);
  const int secs = _wtoi(argv[3]);

  const wchar_t* CLS = L"LpLayerTest";
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = CLS;
  RegisterClassExW(&wc);

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);

  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                              CLS, L"layertest", WS_POPUP, 0, 0, sw, sh,
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) { printf("建窗失败 %lu\n", (unsigned long)GetLastError()); return 2; }

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = sw;
  bi.bmiHeader.biHeight = -sh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) { printf("建 DIB 失败\n"); return 3; }

  /* 预乘语义：alpha=0 时 RGB 该不该被乘掉？这正是要测的。
     填成纯色（RGB 全同），读回来一看就知道。 */
  BYTE* p = (BYTE*)bits;
  for (size_t i = 0; i < (size_t)sw * sh; i++) {
    p[i * 4 + 0] = (BYTE)rgb;      // B
    p[i * 4 + 1] = (BYTE)rgb;      // G
    p[i * 4 + 2] = (BYTE)rgb;      // R
    p[i * 4 + 3] = (BYTE)alpha;    // A
  }

  HGDIOBJ old = SelectObject(mem, bmp);
  POINT src = {0, 0}, dst = {0, 0};
  SIZE sz = {sw, sh};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  const BOOL ok = UpdateLayeredWindow(hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
  printf("[layertest] UpdateLayeredWindow=%d err=%lu  alpha=%d rgb=%d 屏幕=%dx%d\n",
         (int)ok, (unsigned long)GetLastError(), alpha, rgb, sw, sh);
  SelectObject(mem, old);

  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, sw, sh, SWP_NOACTIVATE | SWP_SHOWWINDOW);

  /* 消息泵，保持窗口可见 */
  const DWORD t0 = GetTickCount();
  MSG msg;
  while (GetTickCount() - t0 < (DWORD)secs * 1000) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(20);
  }
  printf("[layertest] 结束\n");

  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  DestroyWindow(hwnd);
  return 0;
}
