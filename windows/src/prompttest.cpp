/*
 * 单行输入框的自动化测试台（prompt.cpp 的验收）。
 *
 * 为什么值得单独写一个 exe：
 *   输入框是这一版里**唯一**用到「原生子控件 + 子类化 + 焦点 + 输入法」的地方，
 *   而它恰恰是最难靠「看一眼截图」验的 —— 截图只能证明它长得对，
 *   证明不了「回车真的触发回调」「空名字真的被拒」「取消真的不回调」
 *   「回调里再开一个输入框不会自己把自己挡住」。
 *
 * 验 5 件事：
 *   ① 正常路径：输入 → 回车 → 回调 ok=true，文本已去首尾空白，窗口已销毁
 *   ② 空名字（只有空格）→ 回车**不关闭**；再按 Esc → 回调 ok=false
 *   ③ Tab 在「输入框 → 确定 → 取消」之间循环
 *   ④ 已经开着一个时再开 → 返回 false，且不覆盖原来的回调
 *   ⑤ ★ 回调里立刻再开一个输入框 → 必须成功
 *      （这条兜的是 finish() 里「先清状态再 DestroyWindow 再回调」那个顺序。
 *        如果顺序写反，第二次 open 会看到 g_hwnd 还在，直接返回 false ——
 *        表现就是「点了重命名没反应」。）
 *
 * 用法：./build/prompttest.exe        （有窗口弹出，约 3 秒）
 * 退出码 0 = 全过。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>

#include "prompt.h"

namespace {

int g_fail = 0;

void check(const char* what, bool ok) {
  printf("  [%s] %s\n", ok ? "通过" : "失败", what);
  if (!ok) g_fail++;
}

struct Res {
  bool got = false;
  bool ok = false;
  std::wstring text;
};
Res g_r;

/* 泵消息，直到 predicate 为真或超时。返回 predicate 最终是否为真。 */
template <typename F>
bool pumpUntil(F pred, int maxMs) {
  MSG m;
  const DWORD t0 = GetTickCount();
  for (;;) {
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    if (pred()) return true;
    if ((int)(GetTickCount() - t0) > maxMs) return false;
    Sleep(5);
  }
}

void pump(int ms) { pumpUntil([] { return false; }, ms); }

HWND findChild(HWND parent, const wchar_t* cls, HWND after) {
  return FindWindowExW(parent, after, cls, nullptr);
}

/* 用例 ① 正常路径 */
void caseNormal() {
  printf("用例① 正常路径（输入 → 回车）\n");
  g_r = Res{};
  const bool opened = lp_prompt_open(nullptr, L"测试 · 正常路径", L"已有名字",
                                     [](bool ok, std::wstring t) {
                                       g_r.got = true;
                                       g_r.ok = ok;
                                       g_r.text = t;
                                     });
  /* ★ 这一句本身就是「非模态」的证明：open() 立刻返回了，
     而不是像 DoModal 那样把控制权交出去。 */
  check("lp_prompt_open 立刻返回 true（非模态）", opened);
  pump(150);

  HWND h = lp_prompt_hwnd();
  check("窗口已创建", h != nullptr && IsWindow(h));
  HWND e = findChild(h, L"EDIT", nullptr);
  check("找到输入框", e != nullptr);

  if (e) {
    DWORD s = 0, t = 0;
    SendMessageW(e, EM_GETSEL, (WPARAM)&s, (LPARAM)&t);
    check("初始文本被全选（直接打字就替换）", s == 0 && t == (DWORD)wcslen(L"已有名字"));

    SetWindowTextW(e, L"  两边留空  ");
    PostMessageW(e, WM_KEYDOWN, VK_RETURN, 0);
    const bool fired = pumpUntil([] { return g_r.got; }, 2000);
    check("回车触发了回调", fired);
    check("回调 ok=true", g_r.ok);
    check("首尾空白被去掉", g_r.text == L"两边留空");
  }
  pumpUntil([] { return lp_prompt_hwnd() == nullptr; }, 1000);
  check("确定后窗口已销毁", lp_prompt_hwnd() == nullptr);
}

/* 用例 ② 空名字被拒 + Esc 取消 */
void caseEmptyAndCancel() {
  printf("用例② 空名字被拒 / Esc 取消\n");
  g_r = Res{};
  lp_prompt_open(nullptr, L"测试 · 空名字", L"",
                 [](bool ok, std::wstring t) { g_r.got = true; g_r.ok = ok; g_r.text = t; });
  pump(150);
  HWND h = lp_prompt_hwnd();
  HWND e = findChild(h, L"EDIT", nullptr);
  if (!e) { check("找到输入框", false); return; }

  SetWindowTextW(e, L"    ");   // 只有空白
  PostMessageW(e, WM_KEYDOWN, VK_RETURN, 0);
  pump(400);
  check("空名字回车**不**回调", !g_r.got);
  check("空名字回车后窗口仍然开着", lp_prompt_hwnd() == h);

  PostMessageW(e, WM_KEYDOWN, VK_ESCAPE, 0);
  const bool fired = pumpUntil([] { return g_r.got; }, 2000);
  check("Esc 触发了回调", fired);
  check("Esc 回调 ok=false", !g_r.ok);
  pumpUntil([] { return lp_prompt_hwnd() == nullptr; }, 1000);
}

/* 用例 ③ Tab 循环 */
void caseTab() {
  printf("用例③ Tab 在三个控件间循环\n");
  lp_prompt_open(nullptr, L"测试 · Tab", L"x",
                 [](bool, std::wstring) {});
  pump(150);
  HWND h = lp_prompt_hwnd();
  HWND e = findChild(h, L"EDIT", nullptr);
  HWND b1 = findChild(h, L"BUTTON", nullptr);
  HWND b2 = b1 ? findChild(h, L"BUTTON", b1) : nullptr;
  if (!e || !b1 || !b2) { check("找到三个子控件", false); lp_prompt_close(); pump(100); return; }
  check("找到三个子控件", true);

  SetFocus(e);
  pump(80);
  const bool focusOk = (GetFocus() == e);
  check("能把焦点给输入框（后续判定有效）", focusOk);

  if (focusOk) {
    PostMessageW(e, WM_KEYDOWN, VK_TAB, 0);
    pump(120);
    check("Tab: 输入框 → 确定", GetFocus() == b1);
    PostMessageW(b1, WM_KEYDOWN, VK_TAB, 0);
    pump(120);
    check("Tab: 确定 → 取消", GetFocus() == b2);
    PostMessageW(b2, WM_KEYDOWN, VK_TAB, 0);
    pump(120);
    check("Tab: 取消 → 回到输入框（循环）", GetFocus() == e);
  }
  lp_prompt_close();
  pump(150);
}

/* 用例 ④ 重复打开被挡 */
void caseAlreadyOpen() {
  printf("用例④ 已开着一个时再开 → 拒绝且不覆盖回调\n");
  g_r = Res{};
  lp_prompt_open(nullptr, L"测试 · 第一个", L"",
                 [](bool ok, std::wstring t) { g_r.got = true; g_r.ok = ok; g_r.text = t; });
  pump(150);
  const bool second = lp_prompt_open(nullptr, L"测试 · 第二个", L"",
                                     [](bool, std::wstring) {});
  check("第二次 open 返回 false", !second);
  /* 原来的回调还在：回车应该触发第一个（把 text 设成「还是我」） */
  HWND h = lp_prompt_hwnd();
  HWND e = findChild(h, L"EDIT", nullptr);
  SetWindowTextW(e, L"还是我");
  PostMessageW(e, WM_KEYDOWN, VK_RETURN, 0);
  pumpUntil([] { return g_r.got; }, 2000);
  check("触发的是原来那个回调", g_r.got && g_r.text == L"还是我");
  pumpUntil([] { return lp_prompt_hwnd() == nullptr; }, 1000);
}

/* 用例 ⑤ ★ 回调里再开一个 */
bool g_reopened = false;
bool g_secondGot = false;
void caseReopenInCallback() {
  printf("用例⑤ 回调里立刻再开一个输入框（状态必须已清干净）\n");
  lp_prompt_open(nullptr, L"测试 · 第一步", L"",
                 [](bool, std::wstring) {
                   g_reopened = lp_prompt_open(nullptr, L"测试 · 第二步", L"",
                                               [](bool, std::wstring) { g_secondGot = true; });
                 });
  pump(150);
  HWND e = findChild(lp_prompt_hwnd(), L"EDIT", nullptr);
  SetWindowTextW(e, L"第一步");
  PostMessageW(e, WM_KEYDOWN, VK_RETURN, 0);
  pumpUntil([] { return g_reopened; }, 2000);
  check("回调里再开成功了（finish 的状态清理顺序对）", g_reopened);
  pump(200);
  check("第二个输入框真的在屏幕上", lp_prompt_hwnd() != nullptr && IsWindow(lp_prompt_hwnd()));
  HWND e2 = findChild(lp_prompt_hwnd(), L"EDIT", nullptr);
  if (e2) {
    SetWindowTextW(e2, L"第二步");
    PostMessageW(e2, WM_KEYDOWN, VK_RETURN, 0);
    pumpUntil([] { return g_secondGot; }, 2000);
  }
  check("第二个的回调也能触发", g_secondGot);
  pumpUntil([] { return lp_prompt_hwnd() == nullptr; }, 1000);
}

}  // namespace

int main() {
  printf("=== 单行输入框测试台 ===\n");
  caseNormal();
  caseEmptyAndCancel();
  caseTab();
  caseAlreadyOpen();
  caseReopenInCallback();
  printf("=== %s（失败 %d 项）===\n", g_fail ? "有失败" : "全部通过", g_fail);
  return g_fail ? 1 : 0;
}
