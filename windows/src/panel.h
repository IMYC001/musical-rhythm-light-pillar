/*
 * 调节面板 —— 自绘的深色面板，外观对齐原作品的**浏览器版**参数面板。
 *
 * 为什么不用原生控件（TRACKBAR / COMBOBOX / BUTTON / PROGRESS）：
 *   浏览器版那套面板是深色毛玻璃 + 自定义滑杆（渐变轨道 + 发光圆钮）+ 胶囊开关，
 *   原生控件是浅灰的 Windows 经典外观，两者放在一起完全不像，用户直接反馈
 *   「太丑太难用」。而原生控件的配色只能靠 WM_CTLCOLOR* 微调，做不到
 *   「圆角、半透明、渐变、发光」这四件事 —— 所以整个面板改成**自己画**。
 *
 * 实现方式：
 *   一个 WS_EX_LAYERED 的弹出窗口（**注意：绝不能带 WS_EX_TRANSPARENT**，
 *   那是覆盖层穿透用的，面板必须能点），配 UpdateLayeredWindow 做逐像素 alpha。
 *   所有内容用 GDI+ 画进一块 32bpp PARGB 的 DIB，画完一次性提交。
 *   好处：圆角外自然透明（ULW 的分层窗口在 alpha=0 处自动命中穿透），
 *   不需要 SetWindowRgn（那条路会把 DirectComposition 的画面一起裁掉）。
 *
 * 控件全是自绘的，所以鼠标交互也得自己处理（见 panelProc）：
 *   滑杆 = 按下跳转 + 拖动 + 松开结束；开关 = 点击切换；下拉 = 面板内浮层列表；
 *   分段 = 点击选择；滚轮 = 滚动内容；右键 = 把该项恢复默认值。
 */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

enum LpItemKind {
  LP_SECTION = 0,   // 分组标题（带下边框）
  LP_SLIDER,        // 滑杆
  LP_SWITCH,        // 胶囊开关
  LP_SELECT,        // 下拉（面板内浮层列表）
  LP_SEGMENT,       // 分段按钮（选项 ≤ 3 个时比下拉顺手）
  LP_BUTTON,        // 普通按钮
  LP_METER,         // 电平条（只读）
  LP_HINT,          // 说明文字（options 里每项一行）
  LP_GAP,
};

struct LpPanelItem {
  LpItemKind kind = LP_SLIDER;
  int id = 0;              // 宿主用来识别「哪一项变了」
  int tab = 0;             // 0 = 声音页，1 = 参数页
  std::wstring label;
  std::wstring unit;       // 数值后缀，如 " dB" / "%" / " 秒"
  int decimals = 0;        // 数值显示小数位
  float dispMul = 1.f;     // 显示值 = 实际值 × dispMul（百分比之类的换算）
  float minv = 0.f, maxv = 1.f, step = 0.01f;
  float deflt = 0.f;       // 右键恢复默认用
  float* fval = nullptr;   // slider 绑定的值
  bool* bval = nullptr;    // switch 绑定的值
  int* ival = nullptr;     // select / segment 绑定的索引
  std::vector<std::wstring> options;

  /* ---- 布局缓存（宿主不要读写）---- */
  float y = 0.f, h = 0.f;
  float selX = 0.f, selW = 0.f;
  float trX = 0.f, trW = 0.f;
  float swX = 0.f, swW = 0.f;
  float valX = 0.f, valW = 0.f;
};

/* 宿主回调：某一项的值被用户改了（宿主负责保存配置 / 触发副作用） */
typedef void (*LpPanelNotify)(int id, void* user);

/* 建面板。items 必须是**稳定**的（宿主持有），面板只保存指针。 */
bool lp_panel_open(HINSTANCE hInst, const std::wstring& title,
                   std::vector<LpPanelItem>* items, LpPanelNotify notify, void* user);
void lp_panel_close();
bool lp_panel_is_open();
HWND lp_panel_hwnd();

void lp_panel_show();
void lp_panel_hide();
void lp_panel_toggle();
/* 外部改了值之后重画（主题自动轮询、托盘菜单等） */
void lp_panel_sync();
/* 主循环每帧调一次：把「节流重绘」攒下的最后一帧补上。
   拖滑杆时鼠标消息一秒可能上千条，不能每条都整块重绘（会和覆盖层抢 CPU，
   表现就是面板卡顿抽搐），所以高频路径走节流 + 每帧补帧。 */
void lp_panel_pump();
void lp_panel_set_tab(int tab);
void lp_panel_set_meter(float v01);
void lp_panel_set_status(const std::wstring& s);
int lp_panel_item_count();
int lp_panel_slider_count();
void lp_panel_reset_all();

/* ---- 自检用（不参与正常交互）---- */
/* 把面板当前渲染结果取出来：BGRA，自上而下，**已反预乘**，alpha 保留 */
bool lp_panel_capture(std::vector<unsigned char>* out, int* w, int* h);
/* 模拟把某个滑杆拖到 [0,1] 的比例位置，返回是否真的改到了值 */
bool lp_panel_test_drag(int id, float frac);
/* 模拟在下拉里选第 index 项 */
bool lp_panel_test_pick(int id, int index);
/* 命中测试：给定窗口坐标，返回落在哪一项上（-1 = 没有） */
int lp_panel_test_hit(int px, int py);
