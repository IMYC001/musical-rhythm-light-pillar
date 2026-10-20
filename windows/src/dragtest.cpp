/* dragtest.exe —— 客观验证「调节面板拖动跟不跟手、抽不抽搐」。
 *
 * 为什么要单独写一个工具，而不是肉眼看着说「好像不卡了」：
 *   拖动抽搐的根因是**坐标基准**（客户区坐标 vs 屏幕坐标），肉眼很难分辨
 *   「窗口跟着光标走」和「窗口围着光标抖」—— 尤其帧率低的时候。
 *   所以这里直接测两个**可证伪的**物理量：
 *
 *   ① 终点误差：光标向右走 N 像素，窗口左边也应该向右走 N 像素。
 *      （用客户区坐标做基准时，窗口每个 drain 只会移动「最后一条消息的增量」，
 *        终点会远远落在光标后面 —— 这是最硬的一条判据。）
 *
 *   ② 回退次数：光标**单调**向右走的时候，窗口左边也应该**单调**不减。
 *      出现「向左退」就是抽搐。修复前一次拖动能退几十次。
 *
 * 用法：
 *   dragtest.exe <窗口标题> <水平位移px> <采样间隔ms> [轨迹输出文件]
 *
 * 退出码：0 = 通过，3 = 判据不达标，2 = 找不到窗口。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>

static double nowMs() {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ★ 不能用 wprintf 直接往管道里写中文。
   stdout 被重定向成管道时，UCRT 会按「C」区域把宽字符转成本地字节 ——
   遇到第一个非 ASCII 字符就转换失败、**后面的内容整段丢掉**，
   实测表现是只打出「[dragtest] 」前缀，数字一个都看不到（白跑一轮）。
   所以自己转 UTF-8 再 fwrite，绕开区域设置。 */
static void out(const wchar_t* fmt, ...) {
  wchar_t buf[2048] = {};
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  char u8[4096] = {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, (int)sizeof(u8) - 1, nullptr, nullptr);
  if (n > 1) fwrite(u8, 1, (size_t)(n - 1), stdout);
  fflush(stdout);
}

static void pressLeft(bool down) {
  INPUT in = {};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
  SendInput(1, &in, sizeof(INPUT));
}

struct Sample {
  double t;
  int cursorX, cursorY;
  int winLeft, winTop, winW, winH;
};

/* 按类名找面板窗口。
   ★ 兜底用：面板标题是「光柱 · 调节面板」，含中点和一个空格，
     从 shell 传进来容易被编码/引号吃掉 —— 标题匹配不上时走这里。 */
struct FindCtx { const wchar_t* cls; HWND found; };
static BOOL CALLBACK findProc(HWND h, LPARAM lp) {
  FindCtx* c = (FindCtx*)lp;
  wchar_t cls[256] = {};
  GetClassNameW(h, cls, 256);
  if (wcscmp(cls, c->cls) == 0 && IsWindowVisible(h)) {
    c->found = h;
    return FALSE;
  }
  return TRUE;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    out(L"用法: dragtest.exe <窗口标题> <水平位移px> <采样间隔ms> [轨迹文件]\n");
    return 1;
  }
  const wchar_t* title = argv[1];
  /* 位移支持 "200" 或 "200x120"（斜向）。竖直方向要单独验一次：
     虽然 x/y 走的是同一段代码，但 y 方向的抓握点更靠近标题栏下沿，
     万一 hit-test 把它判成标签页行，拖动就会变成「切标签页」而不是移动窗口。 */
  int travel = 0, travelY = 0;
  {
    const wchar_t* s = argv[2];
    const wchar_t* xp = wcschr(s, L'x');
    if (xp) {
      travel = _wtoi(s);
      travelY = _wtoi(xp + 1);
    } else {
      travel = _wtoi(s);
    }
  }
  const int stepMs = _wtoi(argv[3]);
  const wchar_t* tracePath = (argc >= 5) ? argv[4] : nullptr;
  if (travel <= 0 || stepMs < 0) {
    out(L"参数不合法。\n");
    return 1;
  }

  HWND h = FindWindowW(nullptr, title);
  if (!h) {
    /* 退一步：按类名找（标题里带了中点和空格，命令行传参容易被 shell 吃掉） */
    FindCtx ctx = {L"LpSettingsPanel", nullptr};
    EnumWindows(findProc, (LPARAM)&ctx);
    h = ctx.found;
    if (h) out(L"[dragtest] 标题没匹配上，按类名 LpSettingsPanel 找到了窗口。\n");
  }
  if (!h) {
    out(L"[dragtest] 找不到标题为「%s」的窗口。\n", title);
    return 2;
  }
  if (!IsWindowVisible(h)) {
    out(L"[dragtest] 窗口存在但不可见，无法拖。\n");
    return 2;
  }

  RECT r0 = {};
  GetWindowRect(h, &r0);
  const int startLeft = r0.left, startTop = r0.top;
  out(L"[dragtest] 窗口句柄 0x%p  初始位置 (%d,%d)  尺寸 %dx%d\n", (void*)h,
          r0.left, r0.top, r0.right - r0.left, r0.bottom - r0.top);

  /* 抓握点：标题栏里、水平居中。
     标题栏高度是 g_headerH（≈38×scale），取 +14 一定落在里面；
     水平居中是为了避开右上角的关闭按钮（那一片点下去是隐藏窗口）。 */
  const int gripX = r0.left + (r0.right - r0.left) / 2;
  const int gripY = r0.top + 14;

  SetCursorPos(gripX, gripY);
  Sleep(150);
  pressLeft(true);
  Sleep(120);

  /* 先确认按下之后窗口没有自己动（真的进了拖动分支） */
  {
    RECT rp = {};
    GetWindowRect(h, &rp);
    if (rp.left != startLeft || rp.top != startTop) {
      out(L"[dragtest] 警告：按下后窗口自己移动了 (%d,%d) → (%d,%d)\n",
              startLeft, startTop, rp.left, rp.top);
    }
  }

  std::vector<Sample> ss;
  ss.reserve((size_t)travel + 8);
  const double t0 = nowMs();

  for (int i = 1; i <= travel; i++) {
    SetCursorPos(gripX + i, gripY + (int)lround((double)travelY * (double)i / (double)travel));
    Sample s = {};
    s.t = nowMs() - t0;
    POINT cp = {};
    GetCursorPos(&cp);
    s.cursorX = cp.x;
    s.cursorY = cp.y;
    RECT r = {};
    GetWindowRect(h, &r);
    s.winLeft = r.left;
    s.winTop = r.top;
    s.winW = r.right - r.left;
    s.winH = r.bottom - r.top;
    ss.push_back(s);
    if (stepMs > 0) Sleep((DWORD)stepMs);
  }

  /* 松手后等一拍，让主循环把队列里最后的消息处理完 */
  Sleep(250);
  pressLeft(false);
  Sleep(250);

  RECT r1 = {};
  /* ★ 必须先确认窗口还在。
     程序 --panelhold 到期会自己退出，句柄一失效 GetWindowRect 就失败、
     结构体保持全 0 —— 结果会被读成「窗口跑到了 (0,0)」，
     看起来像拖动把窗口甩飞了，其实只是被拖的对象没了（白排查一轮）。
     实测：Sleep(1) 在 Windows 上实际约 15ms，200 步要 3 秒，
     很容易撞上存活时间到点。 */
  if (!IsWindow(h)) {
    out(L"[dragtest] 窗口在测量结束前就消失了（--panelhold 到期？）—— 本次结果作废。\n");
    return 4;
  }
  GetWindowRect(h, &r1);
  const int actualOffset = r1.left - startLeft;
  const int errPx = actualOffset - travel;

  /* ---- 判据 ②：回退次数 ----
     只看窗口**左边**。光标是单调向右的，窗口左边就不该往回走。
     允许 1px 的取整抖动，超过才算「回退」。 */
  int backSteps = 0, maxBack = 0, moves = 0;
  for (size_t i = 1; i < ss.size(); i++) {
    const int d = ss[i].winLeft - ss[i - 1].winLeft;
    if (d != 0) moves++;
    if (d < 0) {
      backSteps++;
      if (-d > maxBack) maxBack = -d;
    }
  }

  /* ---- 判据 ③：跟手滞后 ----
     窗口左边与光标之间的间隙应当**恒定**（= 抓握点在窗口内的偏移）。
     取整个拖动过程中的最大偏离。 */
  const int gap0 = gripX - startLeft;
  int maxGapDev = 0;
  for (const Sample& s : ss) {
    const int gap = s.cursorX - s.winLeft;
    const int dev = gap - gap0;
    if (dev > maxGapDev) maxGapDev = dev;
    if (-dev > maxGapDev) maxGapDev = -dev;
  }

  /* ---- 判据 ④：垂直方向 ----
     水平拖动时窗口上边不该动；斜向拖动时应当跟着走。
     按「该走到哪」逐点比，而不是只看终点 —— 只看终点会漏掉中途的漂移。 */
  int maxDy = 0;
  for (size_t i = 0; i < ss.size(); i++) {
    const int want = (int)lround((double)travelY * (double)(i + 1) / (double)travel);
    const int dev = (ss[i].winTop - startTop) - want;
    if (dev > maxDy) maxDy = dev;
    if (-dev > maxDy) maxDy = -dev;
  }
  const int actualOffsetY = r1.top - startTop;
  const int errY = actualOffsetY - travelY;

  out(L"[dragtest] 光标位移 (%d, %d) px\n", travel, travelY);
  out(L"[dragtest] 窗口终点位移 (%d, %d) px → 终点误差 (%d, %d) px\n",
          actualOffset, actualOffsetY, errPx, errY);
  out(L"[dragtest] 采样 %zu 次，其中窗口位置发生变化 %d 次\n", ss.size(), moves);
  out(L"[dragtest] 回退（向左跳）%d 次，最大回退 %d px\n", backSteps, maxBack);
  out(L"[dragtest] 与光标的间隙最大偏离 %d px\n", maxGapDev);
  out(L"[dragtest] 垂直方向最大偏差 %d px（水平拖动应为 0）\n", maxDy);

  bool pass = true;
  if (errPx < -3 || errPx > 3) pass = false;
  if (errY < -3 || errY > 3) pass = false;
  if (maxDy > 6) pass = false;
  /* 中途指标只在「光标走得比程序反应得慢」时有意义。
     stepMs=0 是一次性把整批移动灌进去 —— 采样全在程序处理之前完成，
     窗口当然还停在原地（这不是 bug，是采样快过了被采样对象）。
     那种模式下只有**终点**能说明问题：程序把队列排空之后，
     窗口该落在光标那里；落在别处就是坐标基准错了。 */
  const bool paced = (stepMs > 0);
  if (paced) {
    if (backSteps > 0) pass = false;
    if (maxGapDev > 6) pass = false;
  } else {
    out(L"[dragtest] （猛拖模式：中途采样快过程序处理，回退/间隙指标不适用，只判终点）\n");
  }

  out(L"[dragtest] 判定：%s\n", pass ? L"通过（跟手、不抽搐）" : L"不通过");

  if (tracePath) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, tracePath, L"wb") == 0 && f) {
      fprintf(f, "t_ms,cursor_x,cursor_y,win_left,win_top\n");
      for (const Sample& s : ss)
        fprintf(f, "%.2f,%d,%d,%d,%d\n", s.t, s.cursorX, s.cursorY, s.winLeft, s.winTop);
      fclose(f);
      out(L"[dragtest] 轨迹已写出：%s\n", tracePath);
    }
  }

  return pass ? 0 : 3;
}
