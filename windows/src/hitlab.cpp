/*
 * hitlab.exe —— 鼠标穿透实验台。
 *
 * 目的：用**实验**回答「一个置顶全屏窗口，到底要带哪些窗口样式/处理，
 *       鼠标点击才能穿过它、落到下面的别的进程窗口上」。
 *
 * 为什么必须做实验：微软文档对 WS_EX_TRANSPARENT 的定义只讲**绘制**
 * （"在窗口下的同级姐妹（由同一线程创建）被绘制之前，不应绘制窗口"），
 * 完全没提命中测试。而 WM_NCHITTEST 回 HTTRANSPARENT 的文档明确限定
 * **同一线程**。两条都不足以推出「跨进程穿透」的结论 —— 只能实测。
 *
 * 执行顺序（确定性握手，见 clickprobe.cpp 顶部注释）：
 *   ① 起 clickprobe —— 它先建一个**置顶**靶窗口，保证不被第三方窗口盖住
 *   ② 等它写 ready
 *   ③ 这里才建覆盖层（后建 + 置顶 → 压在靶上面）
 *   ④ 写 go，clickprobe 注入点击
 *   ⑤ 收结果
 *
 * 用法：
 *   hitlab.exe <exStyleHex> <mode> <结果文件>
 *
 *   exStyleHex : 附加到基础样式上的扩展样式位（十六进制，可带 0x）
 *                基础固定为 WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW
 *   mode       : 0 = 什么都不做
 *                1 = WM_NCHITTEST 回 HTTRANSPARENT
 *                2 = UpdateLayeredWindow 铺全透明位图（逐像素 alpha=0）
 *                3 = 分层 + SetLayeredWindowAttributes(LWA_ALPHA, alpha=0)
 *                4 = 分层 + SetLayeredWindowAttributes(LWA_ALPHA, alpha=255)
 *                5 = 分层 + SetLayeredWindowAttributes(LWA_COLORKEY, 黑为键)
 *                6 = UpdateLayeredWindow 铺**不透明**位图（阴性对照：应该挡住）
 *                7 = SetWindowRgn 空区域（文档推荐的"零命中面积"做法）
 *   mode 2..6 会自动补上 WS_EX_LAYERED（否则这些分层 API 不生效）
 *
 * 判读：
 *   靶窗口 downs=1  → 穿透成功
 *   靶窗口 downs=0  → 穿透失败（点击被覆盖层吃掉了）
 *   覆盖层 nchit=0  → 系统在命中测试阶段就跳过了它（这是我们要的）
 *   覆盖层 nchit>0  → 系统仍然命中测试了它
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static int g_hit = 0, g_down = 0, g_ncDown = 0, g_activate = 0;
static int g_mode = 0;
static bool g_paintBlack = false;
static const wchar_t* CLS = L"LpHitLabOverlay";

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_NCHITTEST:
      g_hit++;
      return (g_mode == 1) ? HTTRANSPARENT : DefWindowProcW(h, m, w, l);
    case WM_LBUTTONDOWN:   g_down++;    return 0;
    case WM_LBUTTONUP:     return 0;
    case WM_NCLBUTTONDOWN: g_ncDown++;  return 0;
    case WM_MOUSEACTIVATE: g_activate++; return MA_NOACTIVATE;
    case WM_ERASEBKGND:    return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HBRUSH b = CreateSolidBrush(g_paintBlack ? RGB(0, 0, 0) : RGB(30, 24, 48));
      FillRect(dc, &rc, b);
      DeleteObject(b);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CLOSE:      return 0;
    case WM_NCACTIVATE: return 1;
    case WM_ACTIVATE:   return 0;
    default:            return DefWindowProcW(h, m, w, l);
  }
}

static void pump(DWORD ms) {
  const DWORD t0 = GetTickCount();
  MSG msg;
  do {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  } while (GetTickCount() - t0 < ms);
}

static bool fileExists(const wchar_t* p) {
  return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* 往窗口上贴一张纯色 DIB —— 给 UpdateLayeredWindow 用 */
static void ulwFill(HWND hwnd, int sw, int sh, BYTE alpha) {
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
  if (bmp && bits) {
    BYTE* p = (BYTE*)bits;
    const BYTE v = (BYTE)(alpha ? 60 : 0);
    for (size_t i = 0; i < (size_t)sw * sh; i++) {
      p[i * 4 + 0] = v; p[i * 4 + 1] = v; p[i * 4 + 2] = v; p[i * 4 + 3] = alpha;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT src = {0, 0}, dst = {0, 0};
    SIZE sz = {sw, sh};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL ok = UpdateLayeredWindow(hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    printf("[hitlab] UpdateLayeredWindow(alpha=%u)=%d err=%lu\n",
           (unsigned)alpha, (int)ok, (unsigned long)GetLastError());
    SelectObject(mem, old);
  }
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 4) {
    printf("用法: hitlab.exe <exStyleHex> <mode 0..7> <结果文件>\n");
    return 1;
  }
  const DWORD extra = (DWORD)wcstoul(argv[1], nullptr, 16);
  g_mode = _wtoi(argv[2]);
  const wchar_t* outPath = argv[3];

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = CLS;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);

  /* ---- ① 先起靶进程（它先建窗口，保证在第三方窗口之上）---- */
  wchar_t dir[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, dir, MAX_PATH);
  wchar_t* slash = wcsrchr(dir, L'\\');
  if (slash) *(slash + 1) = 0;

  wchar_t probeExe[MAX_PATH];
  wcscpy_s(probeExe, dir);
  wcscat_s(probeExe, L"clickprobe.exe");

  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  wchar_t resPath[MAX_PATH], readyPath[MAX_PATH], goPath[MAX_PATH];
  wcscpy_s(resPath, tmp);   wcscat_s(resPath, L"hitlab_result.txt");
  wcscpy_s(readyPath, tmp); wcscat_s(readyPath, L"hitlab_ready.txt");
  wcscpy_s(goPath, tmp);    wcscat_s(goPath, L"hitlab_go.txt");
  DeleteFileW(resPath);
  DeleteFileW(readyPath);
  DeleteFileW(goPath);

  const int tw = 240, th = 160;
  const int tx = (sw - tw) / 2, ty = (sh - th) / 2;

  wchar_t cmd[2048];
  _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%ls\" %d %d %d %d \"%ls\" \"%ls\" \"%ls\"",
               probeExe, tx, ty, tw, th, resPath, readyPath, goPath);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    printf("[hitlab] 起 clickprobe 失败 %lu\n", (unsigned long)GetLastError());
    return 4;
  }
  CloseHandle(pi.hThread);

  /* ---- ② 等靶窗口就位 ---- */
  const DWORD tw0 = GetTickCount();
  while (!fileExists(readyPath) && GetTickCount() - tw0 < 15000) pump(50);
  if (!fileExists(readyPath)) { printf("[hitlab] 等 ready 超时\n"); return 5; }
  printf("[hitlab] 靶已就位\n");

  /* ---- ③ 现在才建覆盖层：后建 + 置顶 → 压在靶上面 ---- */
  DWORD ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | extra;
  if (g_mode >= 2 && g_mode <= 6) ex |= WS_EX_LAYERED;

  HWND hwnd = CreateWindowExW(ex, CLS, L"hitlab overlay", WS_POPUP,
                              0, 0, sw, sh, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) { printf("CreateWindowExW 失败 %lu\n", (unsigned long)GetLastError()); return 3; }

  if (g_mode == 2)      ulwFill(hwnd, sw, sh, 0);
  else if (g_mode == 6) ulwFill(hwnd, sw, sh, 255);
  else if (g_mode == 3) { SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);   printf("[hitlab] LWA_ALPHA alpha=0\n"); }
  else if (g_mode == 4) { SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA); printf("[hitlab] LWA_ALPHA alpha=255\n"); }
  else if (g_mode == 5) { g_paintBlack = true; SetLayeredWindowAttributes(hwnd, RGB(0,0,0), 0, LWA_COLORKEY); printf("[hitlab] LWA_COLORKEY 黑色为键（窗口纯黑）\n"); }
  else if (g_mode == 7) { HRGN e = CreateRectRgn(0, 0, 0, 0); SetWindowRgn(hwnd, e, TRUE); printf("[hitlab] SetWindowRgn(空区域)\n"); }

  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, sw, sh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  pump(600);

  LONG_PTR actual = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  printf("[hitlab] exStyle 请求=0x%08lX 实际=0x%08lX  mode=%d 屏幕=%dx%d 覆盖层 hwnd=%p\n",
         (unsigned long)ex, (unsigned long)actual, g_mode, sw, sh, (void*)hwnd);

  /* ---- ④ 放行，让靶注入点击 ---- */
  FILE* gf = _wfopen(goPath, L"wb");
  if (gf) { fputs("go", gf); fclose(gf); }

  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) break;
  }
  CloseHandle(pi.hProcess);

  char probe[768] = "(没读到)";
  FILE* rf = _wfopen(resPath, L"rb");
  if (rf) {
    size_t n = fread(probe, 1, sizeof(probe) - 1, rf);
    probe[n] = 0;
    fclose(rf);
  }
  while (strlen(probe) && (probe[strlen(probe) - 1] == '\n' || probe[strlen(probe) - 1] == '\r'))
    probe[strlen(probe) - 1] = 0;

  int targetDowns = -1;
  if (const char* p = strstr(probe, "downs=")) targetDowns = atoi(p + 6);

  unsigned long long hitVal = 0;
  if (const char* p = strstr(probe, "hitUnderCursor=0x")) hitVal = _strtoui64(p + 16, nullptr, 16);
  const bool hitIsOverlay = (hitVal == (unsigned long long)(ULONG_PTR)hwnd);

  const bool pass = (targetDowns > 0);
  printf("[hitlab] 覆盖层收到: nchit=%d lbuttondown=%d nclbuttondown=%d activate=%d\n",
         g_hit, g_down, g_ncDown, g_activate);
  printf("[hitlab] 靶窗口: %s\n", probe);
  printf("[hitlab] 光标下窗口=0x%llX  是覆盖层自己=%d\n", hitVal, (int)hitIsOverlay);
  printf("[hitlab] === 判定: %s ===\n", pass ? "穿透成功" : "穿透失败（点击被吃掉）");

  FILE* f = _wfopen(outPath, L"wb");
  if (f) {
    fprintf(f, "exStyleRequested=0x%08lX exStyleActual=0x%08lX mode=%d\n",
            (unsigned long)ex, (unsigned long)actual, g_mode);
    fprintf(f, "overlay_hwnd=0x%llX\n", (unsigned long long)(ULONG_PTR)hwnd);
    fprintf(f, "overlay_nchit=%d overlay_lbuttondown=%d overlay_nclbuttondown=%d overlay_activate=%d\n",
            g_hit, g_down, g_ncDown, g_activate);
    fprintf(f, "target_%s\n", probe);
    fprintf(f, "hitIsOverlay=%d\n", (int)hitIsOverlay);
    fprintf(f, "verdict=%s\n", pass ? "PASS" : "FAIL");
    fclose(f);
  }

  DestroyWindow(hwnd);
  return 0;
}
