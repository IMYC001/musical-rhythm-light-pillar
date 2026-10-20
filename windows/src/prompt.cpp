/*
 * 单行文本输入弹窗（见 prompt.h 的说明）。
 *
 * 实现要点：
 *   · 普通窗口（WS_POPUP + 标题栏）+ 原生 EDIT + 两个 BUTTON。
 *     **不用模态循环** —— 只创建窗口就返回，消息由主循环统一泵，
 *     这样打字的时候覆盖层照常跳、音频照常 tick。
 *   · 三个子控件都挂同一个子类化过程，统一处理 回车=确定 / Esc=取消 / Tab=切换焦点。
 *     回车必须自己拦：单行 EDIT 收到回车会「叮」一声（WM_CHAR 的 \r 没人处理），
 *     而且它不是对话框的子控件，DefWindowProc 不会把它变成 IDOK。
 *   · 字体用 SPI_GETNONCLIENTMETRICS 的 lfMessageFont（系统 UI 字体），
 *     中文才不会掉成方框。字号按窗口所在显示器的 DPI 缩放
 *     （进程是 PER_MONITOR_AWARE_V2，所以用 GetDpiForWindow 而不是系统 DPI）。
 */
#define WIN32_LEAN_AND_MEAN
/* ⚠ 必须自己保证 UNICODE 宏：主程序是带 -municode 编的，但 prompt.cpp 也会被
   测试台（prompttest.exe，不带 -municode）链接。没有 UNICODE 时 IDC_ARROW
   展开成 ANSI 版 MAKEINTRESOURCE，LoadCursorW 直接编译不过。 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <utility>

#include "prompt.h"

namespace {

const wchar_t* kPromptClass = L"LpPromptWindow";

const int ID_EDIT = 1001;
const int ID_OK = 1002;
const int ID_CANCEL = 1003;

const UINT EM_SETCUEBANNER_ = 0x1501;   // mingw 的头里不一定有，自己定义

HWND g_hwnd = nullptr;
HWND g_edit = nullptr;
HWND g_okBtn = nullptr;
HWND g_cancelBtn = nullptr;
LpPromptDone g_done;
HFONT g_font = nullptr;
bool g_classReady = false;

struct Hook {
  HWND h = nullptr;
  WNDPROC old = nullptr;
};
Hook g_hooks[3];

std::wstring trimW(const std::wstring& s) {
  size_t a = 0, b = s.size();
  auto sp = [](wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
           c == 0x3000;   // 全角空格
  };
  while (a < b && sp(s[a])) a++;
  while (b > a && sp(s[b - 1])) b--;
  return s.substr(a, b - a);
}

float scaleFor(HWND owner) {
  UINT dpi = 0;
  HMODULE u = GetModuleHandleW(L"user32.dll");
  if (u) {
    typedef UINT(WINAPI * PFN)(HWND);
    PFN f = (PFN)GetProcAddress(u, "GetDpiForWindow");
    if (f && owner) dpi = f(owner);
  }
  if (!dpi) {
    HDC dc = GetDC(nullptr);
    if (dc) {
      dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
      ReleaseDC(nullptr, dc);
    }
  }
  if (dpi < 72) dpi = 96;
  return (float)dpi / 96.0f;
}

HFONT uiFont(HWND owner) {
  if (g_font) return g_font;
  const float sc = scaleFor(owner);
  LOGFONTW lf = {};
  NONCLIENTMETRICSW ncm = {};
  ncm.cbSize = sizeof(ncm);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
    lf = ncm.lfMessageFont;
  } else {
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, L"Microsoft YaHei UI");
  }
  lf.lfHeight = -(LONG)(10.5f * sc * 96.0f / 72.0f + 0.5f);   // 10.5pt
  lf.lfWidth = 0;
  g_font = CreateFontIndirectW(&lf);
  return g_font;
}

void finish(bool ok, std::wstring text) {
  HWND h = g_hwnd;
  if (!h) return;
  LpPromptDone cb = std::move(g_done);
  g_done = nullptr;
  /* 先清干净状态再销毁窗口，再回调 —— 回调里很可能又要开一个输入框
     （比如「保存参数」→ 起名 → 又点重命名），状态没清就会自己把自己挡住。 */
  g_hwnd = nullptr;
  g_edit = nullptr;
  g_okBtn = nullptr;
  g_cancelBtn = nullptr;
  for (auto& k : g_hooks) { k.h = nullptr; k.old = nullptr; }
  DestroyWindow(h);
  if (cb) cb(ok, std::move(text));
}

void accept() {
  if (!g_edit) return;
  wchar_t buf[160] = {};
  GetWindowTextW(g_edit, buf, 159);
  std::wstring s = trimW(buf);
  if (s.empty()) {
    /* 空名字不关闭：留在窗口里并提示，比关掉之后报错好得多。 */
    MessageBeep(MB_ICONWARNING);
    SetFocus(g_edit);
    return;
  }
  finish(true, s);
}

LRESULT CALLBACK ctlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  WNDPROC old = nullptr;
  for (const auto& k : g_hooks) {
    if (k.h == h) old = k.old;
  }
  if (m == WM_KEYDOWN) {
    if (w == VK_ESCAPE) { PostMessageW(g_hwnd, WM_COMMAND, ID_CANCEL, 0); return 0; }
    if (w == VK_RETURN) { PostMessageW(g_hwnd, WM_COMMAND, ID_OK, 0); return 0; }
    if (w == VK_TAB) {
      HWND f = GetFocus();
      HWND nx = (f == g_edit) ? g_okBtn : ((f == g_okBtn) ? g_cancelBtn : g_edit);
      if (nx) SetFocus(nx);
      return 0;
    }
  }
  /* 吃掉「回车/制表/Esc」对应的字符消息：单行 EDIT 收到 \r 会响一声。 */
  if (m == WM_CHAR && (w == L'\r' || w == L'\n' || w == 27 || w == L'\t')) return 0;
  return old ? CallWindowProcW(old, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void hookCtl(HWND h) {
  for (auto& k : g_hooks) {
    if (k.h) continue;
    k.h = h;
    k.old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ctlProc);
    return;
  }
}

LRESULT CALLBACK promptProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_COMMAND: {
      const int id = LOWORD(w);
      if (id == ID_OK) { accept(); return 0; }
      if (id == ID_CANCEL) { finish(false, L""); return 0; }
      break;
    }
    case WM_CLOSE:
      finish(false, L"");
      return 0;
    case WM_DESTROY:
      if (g_hwnd == h) {
        g_hwnd = nullptr;
        g_edit = nullptr;
        g_okBtn = nullptr;
        g_cancelBtn = nullptr;
      }
      return 0;
    default: break;
  }
  return DefWindowProcW(h, m, w, l);
}

bool ensureClass() {
  if (g_classReady) return true;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = promptProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = kPromptClass;
  if (!RegisterClassExW(&wc)) {
    /* 类已注册（比如上一次没销毁干净）也算成功 */
    if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  }
  g_classReady = true;
  return true;
}

}  // namespace

bool lp_prompt_open(HWND owner, const std::wstring& title,
                    const std::wstring& initial, LpPromptDone done) {
  if (g_hwnd) {
    SetForegroundWindow(g_hwnd);
    if (g_edit) SetFocus(g_edit);
    return false;
  }
  if (!ensureClass()) return false;

  const float sc = scaleFor(owner);
  const int pad = (int)(14 * sc + 0.5f);
  const int eh = (int)(26 * sc + 0.5f);
  const int bh = (int)(26 * sc + 0.5f);
  const int bw = (int)(80 * sc + 0.5f);
  const int gap = (int)(8 * sc + 0.5f);
  const int cw = (int)(344 * sc + 0.5f);
  const int ch = pad + eh + (int)(10 * sc) + bh + pad;

  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  const DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  RECT r = {0, 0, cw, ch};
  AdjustWindowRectEx(&r, style, FALSE, ex);
  const int ww = r.right - r.left, wh = r.bottom - r.top;

  int x, y;
  RECT o = {};
  if (owner && IsWindow(owner) && GetWindowRect(owner, &o)) {
    x = o.left + ((o.right - o.left) - ww) / 2;
    y = o.top + ((o.bottom - o.top) - wh) / 3;
  } else {
    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    x = wa.left + ((wa.right - wa.left) - ww) / 2;
    y = wa.top + ((wa.bottom - wa.top) - wh) / 3;
  }

  g_done = std::move(done);
  g_hwnd = CreateWindowExW(ex, kPromptClass, title.c_str(), style, x, y, ww, wh,
                           owner, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!g_hwnd) {
    g_done = nullptr;
    return false;
  }

  HINSTANCE hi = GetModuleHandleW(nullptr);
  HFONT f = uiFont(owner);

  g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                           pad, pad, cw - pad * 2, eh, g_hwnd,
                           (HMENU)(INT_PTR)ID_EDIT, hi, nullptr);
  g_okBtn = CreateWindowExW(0, L"BUTTON", L"确定",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            cw - pad - bw - gap - bw, pad + eh + (int)(10 * sc), bw, bh,
                            g_hwnd, (HMENU)(INT_PTR)ID_OK, hi, nullptr);
  g_cancelBtn = CreateWindowExW(0, L"BUTTON", L"取消",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                cw - pad - bw, pad + eh + (int)(10 * sc), bw, bh,
                                g_hwnd, (HMENU)(INT_PTR)ID_CANCEL, hi, nullptr);

  if (!g_edit || !g_okBtn || !g_cancelBtn) {
    finish(false, L"");
    return false;
  }

  for (HWND c : {g_edit, g_okBtn, g_cancelBtn}) {
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    hookCtl(c);
  }
  SendMessageW(g_edit, EM_SETCUEBANNER_, TRUE, (LPARAM)L"给预设起个名字");
  SendMessageW(g_edit, EM_SETSEL, 0, -1);

  ShowWindow(g_hwnd, SW_SHOW);
  SetForegroundWindow(g_hwnd);
  SetFocus(g_edit);
  return true;
}

bool lp_prompt_is_open() { return g_hwnd != nullptr; }

HWND lp_prompt_hwnd() { return g_hwnd; }

void lp_prompt_close() { finish(false, L""); }
