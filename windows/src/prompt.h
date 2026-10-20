/*
 * 单行文本输入弹窗 —— 用来给「预设」起名字。
 *
 * 为什么不自己在自绘面板里画一个输入框：
 *   预设名要能打中文。自绘窗口要自己处理输入法（WM_IME_* + 候选框定位 +
 *   组字串绘制），代价远大于收益；而一个普通窗口 + 原生 EDIT 控件，
 *   输入法、光标、选区、Ctrl+A/C/V、右键菜单**全都是白送的**。
 *
 * ⚠ 必须是**非模态**的：
 *   模态循环（`while (GetMessage())` 自转）会把主循环停住 —— 覆盖层不重画、
 *   音频不 tick，用户打字的时候壁纸就僵在那里。这里只创建窗口就返回，
 *   消息由主循环那个 `PeekMessageW(&m, nullptr, ...)` 统一泵（它取所有窗口的消息）。
 *
 * 同一时刻只允许一个输入框（g_hwnd 非空就是已经有开着的）。
 */
#pragma once

#include <windows.h>

#include <functional>
#include <string>

/* ok = true 表示用户点了「确定」（或按了回车）；text 是**去掉首尾空白后**的内容。
   ok = false 表示取消 / 关窗。空内容不会被当成确定 —— 会留在窗口里并提示。 */
typedef std::function<void(bool ok, std::wstring text)> LpPromptDone;

/* 打开。已经有开着的就返回 false。owner 只用来定位与置顶，不设 owner 也行。 */
bool lp_prompt_open(HWND owner, const std::wstring& title,
                    const std::wstring& initial, LpPromptDone done);

bool lp_prompt_is_open();
HWND lp_prompt_hwnd();

/* 相当于用户点了「取消」：不回调 done 里的 ok=true */
void lp_prompt_close();
