/*
 * 点击靶：验证「鼠标能不能真的点到覆盖层下面的窗口」。
 *
 * 为什么必须另起一个进程：覆盖层是置顶全屏的，它下面全是**别的进程**
 * （桌面 / 任务栏 / 别的程序）。如果在自己进程里放一个测试窗口，
 * 即使穿透是坏的，点击也可能落到它身上 —— 测出来永远是通过。
 * 必须跨进程才是真实场景。
 *
 * ★ 确定性握手（v3 加）：
 *   之前让 hitlab 先建覆盖层、再起 clickprobe，结果批次运行时经常
 *   出现「覆盖层没收到点击、靶窗口也没收到」——点击被某个**第三方窗口**
 *   截胡了。原因不可控：靶窗口不一定在 Z 序顶端。
 *   现在反过来：
 *     ① 靶窗口先建，而且是 WS_EX_TOPMOST —— 保证它一定在别人上面
 *     ② 写 ready 文件，等 hitlab 建好覆盖层（覆盖层后建 → 压在靶上面）
 *     ③ hitlab 写 go 文件，这里才注入点击
 *   顺序确定，Z 序确定，没有第三方窗口能插进来。
 *
 * ★ 另一个很贵的坑（第一版）：
 *   `MOUSEEVENTF_ABSOLUTE` 的 dx/dy 不是像素，是**归一化到 0..65535** 的值
 *   （映射范围由 MOUSEEVENTF_VIRTUALDESK 决定）。第一版直接把像素坐标
 *   塞进 dx/dy，(820,480) 被解释成屏幕的 (820/65535, 480/65535) ≈ 左上角
 *   —— 点击根本没落到靶窗口上，对照组也是 0，于是被误判成
 *   「本环境丢弃注入输入」。一个假阴性差点让整条验证作废。
 *
 * 用法：clickprobe.exe <x> <y> <宽> <高> <结果文件> <ready文件> <go文件>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_down = 0, g_up = 0, g_move = 0;
static int g_ncDown = 0, g_ncUp = 0, g_ncMove = 0;
static int g_hit = 0;
static const wchar_t* CLS = L"LpClickProbeWindow";

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_NCHITTEST:     g_hit++;  return HTCLIENT;
    case WM_LBUTTONDOWN:   g_down++; return 0;
    case WM_LBUTTONUP:     g_up++;   return 0;
    case WM_MOUSEMOVE:     g_move++; return 0;
    case WM_NCLBUTTONDOWN: g_ncDown++; return 0;
    case WM_NCLBUTTONUP:   g_ncUp++;   return 0;
    case WM_NCMOUSEMOVE:   g_ncMove++; return 0;
    case WM_CLOSE:         return 0;
    default:               return DefWindowProcW(h, m, w, l);
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
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES;
}

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 8) {
    printf("用法: clickprobe.exe <x> <y> <宽> <高> <结果文件> <ready文件> <go文件>\n");
    return 1;
  }
  const int x = _wtoi(argv[1]), y = _wtoi(argv[2]);
  const int w = _wtoi(argv[3]), h = _wtoi(argv[4]);
  const wchar_t* outPath = argv[5];
  const wchar_t* readyPath = argv[6];
  const wchar_t* goPath = argv[7];

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
  wc.lpszClassName = CLS;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;

  /* ★ 置顶：保证靶窗口一定在别人上面，不会被第三方窗口盖住。
     覆盖层会在我们**之后**创建，同样置顶 → 稳稳压在我们上面。 */
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, CLS,
                              L"光柱自检 · 点击靶", WS_POPUP,
                              x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) return 3;
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  UpdateWindow(hwnd);

  RECT wr = {};
  GetWindowRect(hwnd, &wr);
  printf("[probe] hwnd=%p visible=%d rect=(%ld,%ld)-(%ld,%ld)\n", (void*)hwnd,
         (int)IsWindowVisible(hwnd), wr.left, wr.top, wr.right, wr.bottom);

  pump(400);

  /* ① 告诉 hitlab「靶已就位，可以建覆盖层了」 */
  FILE* rf = _wfopen(readyPath, L"wb");
  if (rf) { fputs("ready", rf); fclose(rf); }
  printf("[probe] 已写 ready，等 go…\n");

  /* ② 等 hitlab 建好覆盖层并放行 */
  const DWORD tw = GetTickCount();
  while (!fileExists(goPath) && GetTickCount() - tw < 15000) pump(50);
  if (!fileExists(goPath)) { printf("[probe] 等 go 超时\n"); return 5; }
  printf("[probe] 收到 go\n");

  /* ③ 注入真实鼠标输入：移动 → 按下 → 抬起 */
  const int cx = x + w / 2, cy = y + h / 2;
  POINT before = {};
  GetCursorPos(&before);
  const BOOL sp = SetCursorPos(cx, cy);
  pump(150);
  POINT after = {};
  GetCursorPos(&after);
  printf("[probe] SetCursorPos=%d err=%lu  光标 (%ld,%ld) -> 目标 (%d,%d) -> 实际 (%ld,%ld)\n",
         (int)sp, (unsigned long)GetLastError(), before.x, before.y, cx, cy, after.x, after.y);

  /* ★ 归一化到 0..65535（虚拟桌面范围），不是像素！ */
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  const LONG nx = (LONG)(((long long)(cx - vx) * 65535) / (vw > 1 ? vw - 1 : 1));
  const LONG ny = (LONG)(((long long)(cy - vy) * 65535) / (vh > 1 ? vh - 1 : 1));

  INPUT in[3] = {};
  in[0].type = INPUT_MOUSE;
  in[0].mi.dx = nx;
  in[0].mi.dy = ny;
  in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  in[1].type = INPUT_MOUSE;
  in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  in[2].type = INPUT_MOUSE;
  in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  SetLastError(0);
  const UINT sent = SendInput(3, in, sizeof(INPUT));
  printf("[probe] SendInput 返回 %u / 3  err=%lu\n", sent, (unsigned long)GetLastError());

  POINT finalPos = {};
  GetCursorPos(&finalPos);
  const bool cursorOk = (finalPos.x == cx && finalPos.y == cy);

  /* 光标下面的窗口到底是谁 —— 系统自己的判定 */
  unsigned long long hitVal = 0;
  {
    POINT p = {cx, cy};
    HWND hit = WindowFromPoint(p);
    hitVal = (unsigned long long)(ULONG_PTR)hit;
    DWORD pid = 0;
    GetWindowThreadProcessId(hit, &pid);
    printf("[probe] WindowFromPoint=%p 是我们自己=%d  pid=%lu  前台=%p\n",
           (void*)hit, hit == hwnd, (unsigned long)pid, (void*)GetForegroundWindow());
  }

  /* 处理一下消息，让点击真的被派发进来 */
  pump(700);
  printf("[probe] 收到 downs=%d ups=%d moves=%d ncdowns=%d ncmoves=%d nchit=%d\n",
         g_down, g_up, g_move, g_ncDown, g_ncMove, g_hit);

  FILE* f = _wfopen(outPath, L"wb");
  if (!f) return 4;
  fprintf(f, "downs=%d ups=%d moves=%d ncdowns=%d ncup=%d ncmoves=%d nchit=%d "
             "sent=%u cursor=%ld,%ld cursorAtCenter=%d hitUnderCursor=0x%llX selfHwnd=0x%llX\n",
          g_down, g_up, g_move, g_ncDown, g_ncUp, g_ncMove, g_hit,
          sent, finalPos.x, finalPos.y, (int)cursorOk, hitVal,
          (unsigned long long)(ULONG_PTR)hwnd);
  fclose(f);

  DestroyWindow(hwnd);
  return 0;
}
