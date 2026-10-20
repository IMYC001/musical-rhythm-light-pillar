/*
 * panel.cpp —— 自绘的深色调节面板。设计规格沿用参考实现的网页面板样式：
 *
 *   面板     296px 宽、14px 圆角、rgba(14,14,20,.80) 底、1px rgba(255,255,255,.12) 边
 *   标签页   声音 / 参数，未选中 rgba(255,255,255,.45)，选中 #dce6ff + 蓝色下划线
 *   分组标题 11.5px 600、rgba(255,255,255,.72)、下边框 rgba(255,255,255,.09)
 *   行标签   12px、rgba(255,255,255,.82)；数值 11px、#9fc4ff、等宽数字
 *   滑杆     4px 轨道 rgba(255,255,255,.14)，填充是蓝→紫渐变，13px 圆钮 #dce6ff
 *            + 3px rgba(120,160,255,.22) 光晕
 *   开关     34×19 胶囊，关 rgba(255,255,255,.16)、开 rgba(120,160,255,.85)，白钮 15px
 *   下拉     28px 高、rgba(255,255,255,.07) 底、8px 圆角
 *   分段     选中 rgba(120,190,255,.28) + 边 rgba(150,210,255,.55)
 *   说明     10.5~11px、rgba(255,255,255,.36)
 *
 * 浏览器那层还有 backdrop-filter 毛玻璃。分层窗口拿不到「后面的桌面」，
 * 做不了真模糊，所以底色比 0.80 更实一点（0.93）来保证文字可读 —— 这是唯一
 * 有意偏离原版的地方，理由写在 kPanelAlpha 那里。
 */

#define WIN32_LEAN_AND_MEAN
#include "panel.h"

#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Gdiplus;

#ifndef ULW_ALPHA
#define ULW_ALPHA 0x00000002
#endif

/* ============================ 主题常量 ============================ */

static const wchar_t* kPanelClass = L"LpSettingsPanel";

/* 面板底色。浏览器是 rgba(14,14,20,.80) + backdrop-filter:blur(22px)。
   分层窗口只能自己画，拿不到背后的桌面，没法做真模糊 —— 0.80 的不透明度
   在没有模糊的情况下会让底下的窗口内容透出来、把文字糊掉，所以这里用 0.93
   换可读性。色相完全照抄（14,14,20）。 */
static const BYTE kPanelAlpha = 238;

static inline Color col(int a, int r, int g, int b) { return Color((BYTE)a, (BYTE)r, (BYTE)g, (BYTE)b); }

static const Color C_TEXT     = col(255, 233, 233, 242);  // #e9e9f2
static const Color C_LABEL    = col(209, 255, 255, 255);  // rgba(255,255,255,.82)
static const Color C_VALUE    = col(255, 159, 196, 255);  // #9fc4ff
static const Color C_SEC      = col(184, 255, 255, 255);  // rgba(255,255,255,.72)
static const Color C_SEC_LINE = col(23, 255, 255, 255);   // rgba(255,255,255,.09)
static const Color C_HINT     = col(102, 255, 255, 255);  // rgba(255,255,255,.40)
static const Color C_HINT2    = col(92, 255, 255, 255);   // rgba(255,255,255,.36)
static const Color C_TAB_OFF  = col(115, 255, 255, 255);  // rgba(255,255,255,.45)
static const Color C_TAB_ON   = col(255, 220, 230, 255);  // #dce6ff
static const Color C_TAB_LINE = col(217, 140, 180, 255);  // rgba(140,180,255,.85)
static const Color C_TRACK    = col(36, 255, 255, 255);   // rgba(255,255,255,.14)
static const Color C_TRACK_BG = col(23, 255, 255, 255);   // rgba(255,255,255,.09)
static const Color C_GRAD_A   = col(242, 140, 180, 255);  // rgba(140,180,255,.95)
static const Color C_GRAD_B   = col(242, 196, 140, 255);  // rgba(196,140,255,.95)
static const Color C_THUMB    = col(255, 220, 230, 255);  // #dce6ff
static const Color C_THUMB_HALO = col(56, 120, 160, 255); // rgba(120,160,255,.22)
static const Color C_SW_OFF   = col(41, 255, 255, 255);   // rgba(255,255,255,.16)
static const Color C_SW_ON    = col(217, 120, 160, 255);  // rgba(120,160,255,.85)
static const Color C_FIELD    = col(18, 255, 255, 255);   // rgba(255,255,255,.07)
static const Color C_FIELD_HI = col(33, 255, 255, 255);   // rgba(255,255,255,.13)
static const Color C_FIELD_BD = col(31, 255, 255, 255);   // rgba(255,255,255,.12)
static const Color C_BTN_BD   = col(23, 255, 255, 255);   // rgba(255,255,255,.09)
static const Color C_BTN_ON   = col(204, 140, 190, 255);  // rgba(140,190,255,.8)
static const Color C_BTN_ONBG = col(56, 120, 170, 255);   // rgba(120,170,255,.22)
static const Color C_SEG_ON   = col(71, 120, 190, 255);   // rgba(120,190,255,.28)
static const Color C_SEG_ONBD = col(140, 150, 210, 255);  // rgba(150,210,255,.55)
static const Color C_SEG_BD   = col(46, 255, 255, 255);   // rgba(255,255,255,.18)
static const Color C_HOVER    = col(20, 255, 255, 255);   // 行悬停

/* ============================ 状态 ============================ */

static HWND g_hwnd = nullptr;
static std::vector<LpPanelItem>* g_items = nullptr;
static LpPanelNotify g_notify = nullptr;
static void* g_notifyUser = nullptr;
static std::wstring g_title;

static int g_tab = 0;
static float g_scroll = 0.f, g_scrollMax = 0.f;
static float g_meter = 0.f;
static std::wstring g_status;

static int g_dragItem = -1;      // 正在拖的滑杆项
static int g_hoverItem = -1;
static bool g_dragPanel = false;
/* ★ 拖动起点必须记**屏幕坐标**，不能记客户区坐标。
 *
 *  原因（这就是「面板拖动卡卡的而且抽搐」的真 bug）：
 *  鼠标消息在队列里存的是**屏幕**坐标，被 GetMessage/PeekMessage 取出时才
 *  换算成客户区坐标 —— 用的是**取出那一刻**的窗口位置。
 *  而我们的主循环一次会把队列里的鼠标消息全部排空，于是：
 *    取出第 1 条 → 按当时的窗口位置算出 dx → 窗口移动
 *    取出第 2 条 → 系统按**已经移动过**的窗口位置重新算客户区坐标
 *                → dx 被抵消掉 → 窗口跳回原位
 *    取出第 3 条 → 又跳回去 …
 *  两条消息交替，面板就围着光标来回抽搐。
 *  屏幕坐标不受窗口移动影响，用 GetMessagePos() 取，问题消失。 */
static POINT g_dragOriginScreen = {0, 0};
static RECT g_dragRect = {0, 0, 0, 0};

/* 面板重绘节流。拖滑杆时鼠标消息可能每秒上千条，每条都整块重绘
   （320×739 的 GDI+ 绘制 + UpdateLayeredWindow 全窗混合）会把主循环拖垮，
   面板和覆盖层抢 CPU，看起来就是「卡卡的而且抽搐」。
   限到最多每 8ms 一次（≈120Hz，比屏幕刷新还快，肉眼无差），
   最后一次用 pending 标记补上，保证松手时画面是最终值。 */
static double g_lastRenderMs = -1e9;
static bool g_renderPending = false;

static int g_openSel = -1;       // 展开的下拉项
static float g_selScroll = 0.f;
static int g_hoverOpt = -1;

static float g_scale = 1.f;

/* 布局缓存 */
static int g_winW = 0, g_winH = 0;
static float g_pad = 12.f, g_headerH = 38.f, g_tabsH = 34.f;
static float g_bodyTop = 0.f, g_bodyH = 0.f;
static float g_statusH = 0.f;   // 底部状态条占的高度（有文字时才占位）
static RECT g_selListRect = {0, 0, 0, 0};

/* GDI+ */
static ULONG_PTR g_gdiToken = 0;
static HBITMAP g_hbm = nullptr;
static HGDIOBJ g_oldBmp = nullptr;
static void* g_bits = nullptr;
static HDC g_memDc = nullptr;
static HCURSOR g_curHand = nullptr, g_curArrow = nullptr;
static Bitmap* g_bmp = nullptr;
static Graphics* g_g = nullptr;
static int g_bufW = 0, g_bufH = 0;
static FontFamily* g_family = nullptr;
static Font* g_fSmall = nullptr;    // 10.5
static Font* g_fBody = nullptr;     // 12
static Font* g_fBodyB = nullptr;    // 12 bold
static Font* g_fSec = nullptr;      // 11.5 bold
static Font* g_fTab = nullptr;      // 12
static Font* g_fTabB = nullptr;     // 12 bold
static Font* g_fVal = nullptr;      // 11
static Font* g_fTitle = nullptr;    // 12.5 bold

/* ============================ 小工具 ============================ */

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static float detectScale() {
  UINT dpi = 0;
  HMODULE u = GetModuleHandleW(L"user32.dll");
  if (u) {
    typedef UINT(WINAPI * PFN)();
    PFN f = (PFN)GetProcAddress(u, "GetDpiForSystem");
    if (f) dpi = f();
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

static void addRoundRect(GraphicsPath* p, const RectF& r, float rad) {
  const float d = rad * 2.f;
  if (d <= 0.f) {
    p->AddRectangle(r);
    return;
  }
  p->AddArc(r.X, r.Y, d, d, 180.f, 90.f);
  p->AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
  p->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
  p->AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
  p->CloseFigure();
}

static void fillRound(Graphics* g, const RectF& r, float rad, const Color& c) {
  GraphicsPath p;
  addRoundRect(&p, r, rad);
  SolidBrush b(c);
  g->FillPath(&b, &p);
}

static void strokeRound(Graphics* g, const RectF& r, float rad, const Color& c, float w) {
  GraphicsPath p;
  addRoundRect(&p, r, rad);
  Pen pen(c, w);
  g->DrawPath(&pen, &p);
}

/* 文字。align: 0=左 1=右 2=居中 */
static void drawText(Graphics* g, const std::wstring& s, const RectF& r, Font* f,
                     const Color& c, int align, bool ellipsis) {
  if (s.empty() || !f) return;
  StringFormat fmt;
  fmt.SetAlignment(align == 1 ? StringAlignmentFar
                              : (align == 2 ? StringAlignmentCenter : StringAlignmentNear));
  fmt.SetLineAlignment(StringAlignmentCenter);
  fmt.SetFormatFlags(StringFormatFlagsNoWrap);
  if (ellipsis) fmt.SetTrimming(StringTrimmingEllipsisCharacter);
  SolidBrush b(c);
  g->DrawString(s.c_str(), -1, f, r, &fmt, &b);
}

static float textW(Graphics* g, const std::wstring& s, Font* f) {
  if (s.empty() || !f) return 0.f;
  RectF box;
  g->MeasureString(s.c_str(), -1, f, PointF(0, 0), &box);
  return box.Width;
}

static std::wstring fmtValue(const LpPanelItem& it, float v) {
  wchar_t buf[64];
  const float d = v * it.dispMul;
  if (it.decimals <= 0) swprintf(buf, 64, L"%.0f", d);
  else if (it.decimals == 1) swprintf(buf, 64, L"%.1f", d);
  else swprintf(buf, 64, L"%.2f", d);
  std::wstring s = buf;
  s += it.unit;
  return s;
}

/* ============================ 布局 ============================ */

static float rowHeight(const LpPanelItem& it, Graphics* g) {
  switch (it.kind) {
    case LP_SECTION: return 36.f;
    case LP_SLIDER:  return 47.f;
    case LP_SWITCH:  return 31.f;
    case LP_SELECT:  return 59.f;
    case LP_SEGMENT: return 59.f;
    case LP_BUTTON:  return 39.f;
    case LP_METER:   return 32.f;
    case LP_GAP:     return 10.f;
    case LP_HINT: {
      /* options 里每项一行，不做自动换行 —— 换行宽度随字体变化，
         在自绘面板里算错就是一行字被切掉，不如由宿主明确给行。 */
      const int n = it.options.empty() ? 1 : (int)it.options.size();
      return (float)n * 17.f + 10.f;
    }
    default: return 20.f;
  }
}

/* 重新算整页布局。返回该页内容总高。 */
static float layoutTab(int tab, Graphics* g) {
  float y = 0.f;
  const float W = (float)g_winW;
  for (auto& it : *g_items) {
    if (it.tab != tab) continue;
    if (it.kind == LP_GAP) {
      it.y = y;
      it.h = rowHeight(it, g);
      y += it.h;
      continue;
    }
    it.y = y;
    it.h = rowHeight(it, g);
    const float inner = W - g_pad * 2.f;

    if (it.kind == LP_SLIDER) {
      it.trX = g_pad;
      it.trW = inner;
    } else if (it.kind == LP_SELECT) {
      it.selX = g_pad;
      it.selW = inner;
    } else if (it.kind == LP_SEGMENT) {
      it.selX = g_pad;
      it.selW = inner;
    } else if (it.kind == LP_SWITCH) {
      it.swW = 34.f;
      it.swX = W - g_pad - it.swW;
    }
    /* 数值显示宽度：按最长可能值预留，避免拖动时数字长度变化导致抖动 */
    if (it.kind == LP_SLIDER) {
      std::wstring a = fmtValue(it, it.maxv), b = fmtValue(it, it.minv);
      it.valW = maxf(textW(g, a, g_fVal), textW(g, b, g_fVal)) + 6.f;
      it.valX = W - g_pad - it.valW;
    }
    y += it.h;
  }
  return y;
}

static float contentHeight(int tab) {
  if (!g_g) return 0.f;
  return layoutTab(tab, g_g);
}

/* 内容区高度上限：跟浏览器一样压到 640 逻辑像素，但也不许超出屏幕 */
static float bodyMaxH() {
  int sh = GetSystemMetrics(SM_CYSCREEN);
  float cap = minf(640.f * g_scale, (float)sh - 190.f * g_scale);
  return maxf(cap, 240.f * g_scale);
}

static void resizeWindowToContent() {
  const float ch = contentHeight(g_tab);
  /* 状态条占一条**独立**的底部横条，不压在内容上 —— 第一版把它画在内容区
     上沿，结果盖住了最后一行的滑杆（截图里「总电平」只剩一半）。 */
  g_statusH = g_status.empty() ? 0.f : 26.f * g_scale;
  g_bodyH = minf(ch, bodyMaxH());
  g_scrollMax = maxf(0.f, ch - g_bodyH);
  if (g_scroll > g_scrollMax) g_scroll = g_scrollMax;

  const int newH = (int)std::ceil(g_headerH + g_tabsH + g_bodyH + g_statusH + 1.f);
  if (newH == g_winH) return;

  /* 面板底部保持不动（向上长高），免得内容一变窗口就跳 */
  RECT r;
  GetWindowRect(g_hwnd, &r);
  const int bottom = r.bottom;
  g_winH = newH;
  int x = r.left, y = bottom - newH;
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  if (y < 0) y = 0;
  if (y + newH > sh) y = imax(0, sh - newH);
  SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, g_winW, g_winH, SWP_NOACTIVATE);
}

/* ============================ 缓冲 ============================ */

static void destroyBuffer() {
  /* ⚠ 顺序：先删 GDI+ 的包装（它只是包住 DIB 内存，不拥有内存），
     再把 DC 里**原来**的位图选回去，最后才 DeleteObject ——
     位图还选在 DC 里时删掉它是未定义行为。 */
  if (g_g) { delete g_g; g_g = nullptr; }
  if (g_bmp) { delete g_bmp; g_bmp = nullptr; }
  if (g_memDc && g_oldBmp) { SelectObject(g_memDc, g_oldBmp); g_oldBmp = nullptr; }
  if (g_hbm) { DeleteObject(g_hbm); g_hbm = nullptr; }
  if (g_memDc) { DeleteDC(g_memDc); g_memDc = nullptr; }
  g_bits = nullptr;
  g_bufW = g_bufH = 0;
}

static bool ensureBuffer(int w, int h) {
  if (w <= 0 || h <= 0) return false;
  if (g_bits && g_bufW == w && g_bufH == h) return true;
  destroyBuffer();

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // 负数 = 自上而下
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  g_hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
  if (!g_hbm || !g_bits) return false;

  HDC screen = GetDC(nullptr);
  g_memDc = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);
  if (!g_memDc) return false;
  g_oldBmp = SelectObject(g_memDc, g_hbm);

  /* ★ 关键：GDI+ 的 Bitmap 直接**包住** DIB 的像素内存（不拷贝），
     而且必须声明成 PixelFormat32bppPARGB —— 这样 GDI+ 画进来的每一笔
     都是**预乘**的，正是 UpdateLayeredWindow 要的格式。
     如果声明成 32bppArgb，画出来的 RGB 不预乘，ULW 合成时会整体偏亮。 */
  g_bmp = new Bitmap(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)g_bits);
  if (!g_bmp || g_bmp->GetLastStatus() != Ok) {
    destroyBuffer();
    return false;
  }
  g_g = new Graphics(g_bmp);
  if (!g_g || g_g->GetLastStatus() != Ok) {
    destroyBuffer();
    return false;
  }
  g_g->SetSmoothingMode(SmoothingModeAntiAlias);
  g_g->SetPixelOffsetMode(PixelOffsetModeHalf);
  g_g->SetCompositingMode(CompositingModeSourceOver);
  g_g->SetCompositingQuality(CompositingQualityHighQuality);
  /* 不用 ClearType：透明底上做次像素抗锯齿会在字形边缘留彩色描边 */
  g_g->SetTextRenderingHint(TextRenderingHintAntiAlias);
  g_bufW = w;
  g_bufH = h;
  return true;
}

static bool ensureFonts() {
  if (g_family) return true;
  const wchar_t* names[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI", L"Arial"};
  for (const wchar_t* n : names) {
    FontFamily* f = new FontFamily(n);
    if (f && f->IsAvailable()) {
      g_family = f;
      break;
    }
    delete f;
  }
  if (!g_family) g_family = FontFamily::GenericSansSerif()->Clone();

  const float S = g_scale;
  g_fSmall = new Font(g_family, 10.5f * S, FontStyleRegular, UnitPixel);
  g_fBody = new Font(g_family, 12.f * S, FontStyleRegular, UnitPixel);
  g_fBodyB = new Font(g_family, 12.f * S, FontStyleBold, UnitPixel);
  g_fSec = new Font(g_family, 11.5f * S, FontStyleBold, UnitPixel);
  g_fTab = new Font(g_family, 12.f * S, FontStyleRegular, UnitPixel);
  g_fTabB = new Font(g_family, 12.f * S, FontStyleBold, UnitPixel);
  g_fVal = new Font(g_family, 11.f * S, FontStyleRegular, UnitPixel);
  g_fTitle = new Font(g_family, 12.5f * S, FontStyleBold, UnitPixel);
  return g_fSmall && g_fBody && g_fSec && g_fVal;
}

/* ============================ 绘制 ============================ */

static void drawTabs() {
  const float W = (float)g_winW;
  const float ty = g_headerH;
  /* 标题行 */
  RectF tr(g_pad, 0, W - g_pad * 2 - 26.f, g_headerH);
  drawText(g_g, g_title, tr, g_fTitle, C_SEC, 0, true);

  /* 关闭按钮 */
  const float cs = 22.f;
  RectF cr(W - g_pad - cs, (g_headerH - cs) * 0.5f, cs, cs);
  fillRound(g_g, cr, 6.f, col(14, 255, 255, 255));
  {
    Pen pen(col(140, 255, 255, 255), 1.4f * g_scale);
    const float k = 5.f * g_scale;
    g_g->DrawLine(&pen, cr.X + cr.Width / 2 - k, cr.Y + cr.Height / 2 - k,
                  cr.X + cr.Width / 2 + k, cr.Y + cr.Height / 2 + k);
    g_g->DrawLine(&pen, cr.X + cr.Width / 2 + k, cr.Y + cr.Height / 2 - k,
                  cr.X + cr.Width / 2 - k, cr.Y + cr.Height / 2 + k);
  }

  /* 标签页 */
  const wchar_t* names[2] = {L"声音", L"参数"};
  const float tw = W / 2.f;
  for (int i = 0; i < 2; i++) {
    const bool on = (g_tab == i);
    RectF r((float)i * tw, ty, tw, g_tabsH);
    drawText(g_g, names[i], r, on ? g_fTabB : g_fTab, on ? C_TAB_ON : C_TAB_OFF, 2, false);
    if (on) {
      RectF u((float)i * tw + 1.f, ty + g_tabsH - 2.f, tw - 2.f, 2.f);
      fillRound(g_g, u, 1.f, C_TAB_LINE);
    }
  }
  RectF line(0, ty + g_tabsH - 1.f, W, 1.f);
  SolidBrush lb(C_SEC_LINE);
  g_g->FillRectangle(&lb, line);
}

static void drawSlider(const LpPanelItem& it) {
  const float top = g_bodyTop - g_scroll + it.y;
  const float cy = top + 27.f;
  RectF lr(g_pad, top, it.valX - g_pad - 4.f, 18.f);
  drawText(g_g, it.label, lr, g_fBody, C_LABEL, 0, true);

  const float v = it.fval ? *it.fval : it.minv;
  RectF vr(it.valX, top, it.valW, 18.f);
  drawText(g_g, fmtValue(it, v), vr, g_fVal, C_VALUE, 1, false);

  /* 轨道 */
  const float th = 4.f * g_scale;
  RectF track(it.trX, cy - th / 2.f, it.trW, th);
  fillRound(g_g, track, th / 2.f, C_TRACK);

  const float span = (it.maxv - it.minv) != 0.f ? (it.maxv - it.minv) : 1.f;
  const float frac = clampf((v - it.minv) / span, 0.f, 1.f);
  const float thumbX = it.trX + frac * it.trW;
  if (frac > 0.001f) {
    RectF fill(it.trX, cy - th / 2.f, thumbX - it.trX, th);
    LinearGradientBrush gb(PointF(it.trX, cy), PointF(it.trX + maxf(it.trW, 1.f), cy),
                           C_GRAD_A, C_GRAD_B);
    GraphicsPath p;
    addRoundRect(&p, fill, th / 2.f);
    g_g->FillPath(&gb, &p);
  }

  /* 圆钮 + 光晕 */
  const float rr = 6.5f * g_scale;
  SolidBrush halo(C_THUMB_HALO);
  g_g->FillEllipse(&halo, thumbX - rr - 3.f * g_scale, cy - rr - 3.f * g_scale,
                   (rr + 3.f * g_scale) * 2.f, (rr + 3.f * g_scale) * 2.f);
  SolidBrush tb(C_THUMB);
  g_g->FillEllipse(&tb, thumbX - rr, cy - rr, rr * 2.f, rr * 2.f);
}

static void drawSwitch(const LpPanelItem& it) {
  const float top = g_bodyTop - g_scroll + it.y;
  RectF lr(g_pad, top, g_winW - g_pad * 2.f - it.swW - 8.f, 18.f);
  drawText(g_g, it.label, lr, g_fBody, C_LABEL, 0, true);

  const bool on = it.bval ? *it.bval : false;
  const float sw = 34.f * g_scale, sh = 19.f * g_scale;
  RectF pill(it.swX, top + (18.f - sh) * 0.5f + 1.f, sw, sh);
  fillRound(g_g, pill, sh / 2.f, on ? C_SW_ON : C_SW_OFF);
  const float kr = 7.5f * g_scale;
  const float kx = on ? (pill.X + sw - 2.f * g_scale - kr) : (pill.X + 2.f * g_scale + kr);
  SolidBrush kb(col(255, 255, 255, 255));
  g_g->FillEllipse(&kb, kx - kr, pill.Y + sh / 2.f - kr, kr * 2.f, kr * 2.f);
}

/* 下拉 / 分段的「当前值」那一行 */
static void drawSelectFace(const LpPanelItem& it, int index, float top) {
  RectF lr(g_pad, top, it.valW, 18.f);
  drawText(g_g, it.label, lr, g_fBody, C_LABEL, 0, true);

  const int sel = it.ival ? *it.ival : 0;
  const std::wstring cur = (sel >= 0 && sel < (int)it.options.size()) ? it.options[sel] : L"";
  RectF vr(g_pad, top, g_winW - g_pad * 2.f - 4.f, 18.f);
  drawText(g_g, cur, vr, g_fVal, C_VALUE, 1, true);

  const float fy = top + 21.f, fh = 28.f * g_scale;
  RectF field(it.selX, fy, it.selW, fh);
  fillRound(g_g, field, 8.f * g_scale, (index == g_openSel) ? C_FIELD_HI : C_FIELD);
  strokeRound(g_g, field, 8.f * g_scale, C_FIELD_BD, 1.f);

  RectF txt(field.X + 9.f * g_scale, field.Y, field.Width - 30.f * g_scale, field.Height);
  drawText(g_g, cur, txt, g_fBody, C_TEXT, 0, true);

  /* 右侧的小箭头 */
  const float ax = field.X + field.Width - 15.f * g_scale, ay = field.Y + field.Height / 2.f;
  const float k = 3.6f * g_scale;
  Pen pen(col(150, 255, 255, 255), 1.5f * g_scale);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  g_g->DrawLine(&pen, ax - k, ay - k * 0.45f, ax, ay + k * 0.6f);
  g_g->DrawLine(&pen, ax, ay + k * 0.6f, ax + k, ay - k * 0.45f);
}

static void drawSegment(const LpPanelItem& it) {
  const float top = g_bodyTop - g_scroll + it.y;
  RectF lr(g_pad, top, it.valW, 18.f);
  drawText(g_g, it.label, lr, g_fBody, C_LABEL, 0, true);
  const int sel = it.ival ? *it.ival : 0;
  const std::wstring cur = (sel >= 0 && sel < (int)it.options.size()) ? it.options[sel] : L"";
  RectF vr(g_pad, top, g_winW - g_pad * 2.f - 4.f, 18.f);
  drawText(g_g, cur, vr, g_fVal, C_VALUE, 1, true);

  const int n = imax(1, (int)it.options.size());
  const float gap = 6.f * g_scale;
  const float bw = (it.selW - gap * (n - 1)) / (float)n;
  const float by = top + 21.f, bh = 28.f * g_scale;
  for (int i = 0; i < n; i++) {
    RectF b(it.selX + (bw + gap) * i, by, bw, bh);
    const bool on = (i == sel);
    fillRound(g_g, b, 7.f * g_scale, on ? C_SEG_ON : C_FIELD);
    strokeRound(g_g, b, 7.f * g_scale, on ? C_SEG_ONBD : C_SEG_BD, 1.f);
    drawText(g_g, it.options[i], b, g_fBody, on ? C_TEXT : col(217, 255, 255, 255), 2, true);
  }
}

static void drawRow(const LpPanelItem& it, int index) {
  const float top = g_bodyTop - g_scroll + it.y;
  /* 越界的行直接跳过（滚动时） */
  if (top + it.h < g_bodyTop - 2.f || top > g_bodyTop + g_bodyH + 2.f) return;

  if (index == g_hoverItem && (it.kind == LP_SLIDER || it.kind == LP_SWITCH ||
                               it.kind == LP_SELECT || it.kind == LP_SEGMENT)) {
    RectF hr(g_pad * 0.4f, top - 3.f, g_winW - g_pad * 0.8f, it.h - 6.f);
    fillRound(g_g, hr, 8.f * g_scale, C_HOVER);
  }

  switch (it.kind) {
    case LP_SECTION: {
      RectF r(g_pad, top + 10.f, g_winW - g_pad * 2.f, 17.f);
      drawText(g_g, it.label, r, g_fSec, C_SEC, 0, true);
      SolidBrush lb(C_SEC_LINE);
      g_g->FillRectangle(&lb, RectF(g_pad, top + 30.f, g_winW - g_pad * 2.f, 1.f));
      break;
    }
    case LP_SLIDER: drawSlider(it); break;
    case LP_SWITCH: drawSwitch(it); break;
    case LP_SELECT: drawSelectFace(it, index, top); break;
    case LP_SEGMENT: drawSegment(it); break;
    case LP_BUTTON: {
      const float bh = 32.f * g_scale;
      RectF b(g_pad, top + 2.f, g_winW - g_pad * 2.f, bh);
      fillRound(g_g, b, 9.f * g_scale, C_FIELD);
      strokeRound(g_g, b, 9.f * g_scale, C_BTN_BD, 1.f);
      RectF t(b.X + 10.f * g_scale, b.Y, b.Width - 20.f * g_scale, b.Height);
      drawText(g_g, it.label, t, g_fBody, C_TEXT, 0, true);
      break;
    }
    case LP_METER: {
      RectF lr(g_pad, top, g_winW - g_pad * 2.f, 14.f);
      drawText(g_g, it.label, lr, g_fSmall, C_HINT, 0, false);
      const float bh = 4.f * g_scale;
      const float by = top + 18.f;
      RectF track(g_pad, by, g_winW - g_pad * 2.f, bh);
      fillRound(g_g, track, bh / 2.f, C_TRACK_BG);
      const float w = clampf(g_meter, 0.f, 1.f) * track.Width;
      if (w > 0.5f) {
        RectF fill(g_pad, by, w, bh);
        LinearGradientBrush gb(PointF(track.X, by), PointF(track.X + track.Width, by),
                               C_GRAD_A, C_GRAD_B);
        GraphicsPath p;
        addRoundRect(&p, fill, bh / 2.f);
        g_g->FillPath(&gb, &p);
      }
      break;
    }
    case LP_HINT: {
      const int n = it.options.empty() ? 1 : (int)it.options.size();
      for (int i = 0; i < n; i++) {
        const std::wstring& s = it.options.empty() ? it.label : it.options[i];
        RectF r(g_pad, top + 4.f + (float)i * 17.f, g_winW - g_pad * 2.f, 17.f);
        drawText(g_g, s, r, g_fSmall, C_HINT2, 0, true);
      }
      break;
    }
    default: break;
  }
}

/* 展开的下拉列表（画在最后，压在所有内容之上） */
static void drawSelectList() {
  if (g_openSel < 0 || g_openSel >= (int)g_items->size()) return;
  const LpPanelItem& it = (*g_items)[g_openSel];
  const int n = (int)it.options.size();
  if (n == 0) return;

  const float rowH = 28.f * g_scale;
  const float lh = (float)g_selListRect.bottom - g_selListRect.top;
  const float x = (float)g_selListRect.left, w = (float)g_selListRect.right - x;
  const float y = (float)g_selListRect.top;

  RectF box(x, y, w, lh);
  fillRound(g_g, box, 10.f * g_scale, col(248, 22, 23, 32));
  strokeRound(g_g, box, 10.f * g_scale, col(38, 255, 255, 255), 1.f);

  const float maxScroll = maxf(0.f, (float)n * rowH + 8.f - lh);
  if (g_selScroll > maxScroll) g_selScroll = maxScroll;

  for (int i = 0; i < n; i++) {
    const float iy = y + 4.f - g_selScroll + (float)i * rowH;
    if (iy + rowH < y + 1.f || iy > y + lh - 1.f) continue;
    RectF r(x + 4.f, iy, w - 8.f, rowH);
    const bool on = (i == *it.ival);
    if (on) fillRound(g_g, r, 7.f * g_scale, C_BTN_ONBG);
    else if (i == g_hoverOpt) fillRound(g_g, r, 7.f * g_scale, C_FIELD_HI);
    RectF t(r.X + 9.f * g_scale, r.Y, r.Width - 18.f * g_scale, r.Height);
    drawText(g_g, it.options[i], t, g_fBody, on ? C_TEXT : col(235, 233, 233, 242), 0, true);
  }

  if (maxScroll > 0.5f) {
    const float trackX = x + w - 6.f * g_scale;
    const float trackY = y + 5.f, trackH = lh - 10.f;
    const float frac = lh / (lh + maxScroll);
    const float th = maxf(20.f * g_scale, trackH * frac);
    const float ty = trackY + (trackH - th) * (maxScroll > 0.f ? g_selScroll / maxScroll : 0.f);
    fillRound(g_g, RectF(trackX, ty, 3.f * g_scale, th), 1.5f * g_scale,
              col(90, 255, 255, 255));
  }
}

static void drawScrollbar() {
  const float ch = contentHeight(g_tab);
  if (g_scrollMax <= 0.5f || ch <= 0.f) return;
  const float trackX = (float)g_winW - 5.f * g_scale;
  const float trackY = g_bodyTop + 2.f, trackH = g_bodyH - 4.f;
  fillRound(g_g, RectF(trackX, trackY, 3.f * g_scale, trackH), 1.5f * g_scale,
            col(16, 255, 255, 255));
  const float frac = g_bodyH / ch;
  const float th = maxf(24.f * g_scale, trackH * frac);
  const float ty = trackY + (trackH - th) * (g_scroll / g_scrollMax);
  fillRound(g_g, RectF(trackX, ty, 3.f * g_scale, th), 1.5f * g_scale,
            col(70, 255, 255, 255));
}

static double panelNowMs() {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void render() {
  if (!g_hwnd || !ensureBuffer(g_winW, g_winH)) return;
  if (!ensureFonts()) return;

  g_g->Clear(Color(0, 0, 0, 0));

  /* 面板底 + 边 */
  RectF outer(0.5f, 0.5f, (float)g_winW - 1.f, (float)g_winH - 1.f);
  fillRound(g_g, outer, 14.f * g_scale, col(kPanelAlpha, 14, 14, 20));
  strokeRound(g_g, outer, 14.f * g_scale, col(31, 255, 255, 255), 1.f);

  drawTabs();

  /* 内容区裁剪 */
  g_bodyTop = g_headerH + g_tabsH;
  {
    Region clip(RectF(0.f, g_bodyTop, (float)g_winW, g_bodyH));
    g_g->SetClip(&clip);
    for (int i = 0; i < (int)g_items->size(); i++) {
      const LpPanelItem& it = (*g_items)[i];
      if (it.tab != g_tab) continue;
      drawRow(it, i);
    }
    g_g->ResetClip();
  }

  drawScrollbar();
  drawSelectList();

  /* 底部状态条：占一条独立的横条（在内容区之外） */
  if (g_statusH > 0.f) {
    const float sy = (float)g_winH - g_statusH - 0.5f;
    SolidBrush b(col(214, 16, 17, 24));
    g_g->FillRectangle(&b, RectF(0.5f, sy, (float)g_winW - 1.f, g_statusH));
    SolidBrush lb(C_SEC_LINE);
    g_g->FillRectangle(&lb, RectF(0.5f, sy, (float)g_winW - 1.f, 1.f));
    RectF t(g_pad, sy + 1.f, g_winW - g_pad * 2.f, g_statusH - 1.f);
    drawText(g_g, g_status, t, g_fSmall, C_HINT, 0, true);
  }

  g_g->Flush(FlushIntentionSync);

  /* 提交到分层窗口 */
  HDC screen = GetDC(nullptr);
  POINT dst = {0, 0};
  RECT wr;
  GetWindowRect(g_hwnd, &wr);
  dst.x = wr.left;
  dst.y = wr.top;
  SIZE sz = {g_winW, g_winH};
  POINT src = {0, 0};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, g_memDc, &src, 0, &bf, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
  g_lastRenderMs = panelNowMs();
  g_renderPending = false;
}

/* 高频路径（拖滑杆 / 悬停切换）用的节流重绘。
   鼠标消息一秒可能上千条，每条都整块重绘会直接和覆盖层的渲染抢 CPU，
   表现就是「面板卡卡的而且抽搐」。这里限到最多每 8ms 一次（≈120Hz，
   比屏幕刷新还快），超出的只记一个 pending 标记，
   由主循环每帧调用 lp_panel_pump() 补上最后那一帧 —— 松手时画面一定是终值。 */
static void renderThrottled() {
  if (!g_hwnd) return;
  if (panelNowMs() - g_lastRenderMs < 8.0) { g_renderPending = true; return; }
  render();
}

/* 主循环每帧调一次：把节流期间攒下的最后一次重绘补上。 */
void lp_panel_pump() {
  if (!g_hwnd || !g_renderPending) return;
  if (panelNowMs() - g_lastRenderMs < 8.0) return;
  render();
}

/* ============================ 命中测试 ============================ */

static int hitItem(int px, int py, float* outLocalY) {
  if (py < g_bodyTop || py > g_bodyTop + g_bodyH) return -1;
  const float ly = (float)py - g_bodyTop + g_scroll;
  for (int i = 0; i < (int)g_items->size(); i++) {
    const LpPanelItem& it = (*g_items)[i];
    if (it.tab != g_tab) continue;
    if (ly >= it.y && ly < it.y + it.h) {
      if (outLocalY) *outLocalY = ly - it.y;
      return i;
    }
  }
  return -1;
}

/* 列表展开时的命中：返回选项下标，-1 = 不在列表里 */
static int hitOption(int px, int py) {
  if (g_openSel < 0) return -1;
  const LpPanelItem& it = (*g_items)[g_openSel];
  const int n = (int)it.options.size();
  const float rowH = 28.f * g_scale;
  const float x = (float)g_selListRect.left, w = (float)g_selListRect.right - x;
  const float y = (float)g_selListRect.top;
  const float lh = (float)g_selListRect.bottom - y;
  if (px < x || px > x + w || py < y || py > y + lh) return -1;
  const int i = (int)std::floor(((float)py - y - 4.f + g_selScroll) / rowH);
  if (i < 0 || i >= n) return -1;
  return i;
}

static void openSelect(int idx) {
  g_openSel = idx;
  g_selScroll = 0.f;
  g_hoverOpt = -1;
  const LpPanelItem& it = (*g_items)[idx];
  const int n = (int)it.options.size();
  const float rowH = 28.f * g_scale;
  const float want = (float)n * rowH + 8.f * g_scale;
  const float lh = minf(want, minf(340.f * g_scale, g_bodyH - 16.f));

  const float absY = g_bodyTop + it.y + 21.f - g_scroll;
  float top = absY + 30.f * g_scale;
  if (top + lh > g_bodyTop + g_bodyH) top = maxf(g_bodyTop + 4.f, absY - lh - 4.f);
  const float x = it.selX, w = it.selW;
  g_selListRect.left = (LONG)x;
  g_selListRect.top = (LONG)top;
  g_selListRect.right = (LONG)(x + w);
  g_selListRect.bottom = (LONG)(top + lh);
}

static void notifyChanged(int id) {
  if (g_notify) g_notify(id, g_notifyUser);
}

/* ============================ 窗口过程 ============================ */

static void applySliderFromX(LpPanelItem& it, float x) {
  const float frac = clampf((x - it.trX) / maxf(it.trW, 1.f), 0.f, 1.f);
  const float span = it.maxv - it.minv;
  float v = it.minv + frac * span;
  if (it.step > 0.f) v = it.minv + std::floor((v - it.minv) / it.step + 0.5f) * it.step;
  v = clampf(v, it.minv, it.maxv);
  if (it.fval) *it.fval = v;
  notifyChanged(it.id);
}

static LRESULT CALLBACK panelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      const int px = (short)LOWORD(l), py = (short)HIWORD(l);

      /* ① 下拉列表展开时，点击先给列表 */
      if (g_openSel >= 0) {
        const int oi = hitOption(px, py);
        if (oi >= 0) {
          LpPanelItem& it = (*g_items)[g_openSel];
          if (it.ival) *it.ival = oi;
          notifyChanged(it.id);
          g_openSel = -1;
          render();
          return 0;
        }
        /* 点在列表外 → 收起来；点在原来那个下拉脸上 → 也算收起 */
        g_openSel = -1;
        render();
      }

      /* ② 标题行：关闭按钮 / 拖动 */
      if (py < (int)g_headerH) {
        const float cs = 22.f;
        if (px >= g_winW - g_pad - cs - 4.f) {
          lp_panel_hide();
          return 0;
        }
        g_dragPanel = true;
        /* 记屏幕坐标（不是客户区坐标）—— 见 g_dragOriginScreen 的注释 */
#ifdef LP_BUG_DRAG
        /* 对照组：故意退回「用客户区坐标」的旧写法。
           只为证明 dragtest.exe 抓得住这个 bug —— 测试抓不住的测试没有意义。 */
        g_dragOriginScreen.x = px;
        g_dragOriginScreen.y = py;
#else
        {
          const DWORD mp = GetMessagePos();
          g_dragOriginScreen.x = (short)LOWORD(mp);
          g_dragOriginScreen.y = (short)HIWORD(mp);
        }
#endif
        GetWindowRect(h, &g_dragRect);
        SetCapture(h);
        return 0;
      }
      /* ③ 标签页 */
      if (py < (int)(g_headerH + g_tabsH)) {
        g_tab = (px < g_winW / 2) ? 0 : 1;
        g_scroll = 0.f;
        resizeWindowToContent();
        render();
        return 0;
      }

      /* ④ 内容 */
      float ly = 0.f;
      const int idx = hitItem(px, py, &ly);
      if (idx < 0) return 0;
      LpPanelItem& it = (*g_items)[idx];
      switch (it.kind) {
        case LP_SLIDER: {
          g_dragItem = idx;
          applySliderFromX(it, (float)px);
          SetCapture(h);
          render();
          break;
        }
        case LP_SWITCH:
          if (it.bval) *it.bval = !*it.bval;
          notifyChanged(it.id);
          render();
          break;
        case LP_SELECT:
          openSelect(idx);
          render();
          break;
        case LP_SEGMENT: {
          const int n = imax(1, (int)it.options.size());
          const float gap = 6.f * g_scale;
          const float bw = (it.selW - gap * (n - 1)) / (float)n;
          int k = (int)std::floor(((float)px - it.selX) / (bw + gap));
          if (k < 0) k = 0;
          if (k >= n) k = n - 1;
          if (it.ival) *it.ival = k;
          notifyChanged(it.id);
          render();
          break;
        }
        case LP_BUTTON:
          notifyChanged(it.id);
          render();
          break;
        default: break;
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      /* 右键 = 把这一项恢复默认值。比「整页恢复默认」细一档，很常用。 */
      const int px = (short)LOWORD(l), py = (short)HIWORD(l);
      const int idx = hitItem(px, py, nullptr);
      if (idx < 0) return 0;
      LpPanelItem& it = (*g_items)[idx];
      if (it.kind == LP_SLIDER && it.fval) *it.fval = it.deflt;
      else if (it.kind == LP_SWITCH && it.bval) *it.bval = it.deflt != 0.f;
      else if ((it.kind == LP_SELECT || it.kind == LP_SEGMENT) && it.ival)
        *it.ival = (int)(it.deflt + 0.5f);
      else return 0;
      notifyChanged(it.id);
      render();
      return 0;
    }
    case WM_MOUSEMOVE: {
      const int px = (short)LOWORD(l), py = (short)HIWORD(l);
      /* ★ 拖动面板必须用**屏幕坐标**（GetMessagePos），不能用 lParam 的客户区坐标。
         消息被取出时才把屏幕坐标换算成客户区坐标，用的是**取出那一刻**的窗口位置；
         而主循环一次会把队列里的鼠标消息全部排空，于是「移动窗口 → 下一条消息
         按新位置重算客户区坐标 → 位移被抵消 → 窗口跳回原位」，
         两条消息交替就变成围着光标来回抽搐。详见 g_dragOriginScreen 的注释。 */
      const DWORD mp = GetMessagePos();
      const int sx = (short)LOWORD(mp), sy = (short)HIWORD(mp);
      if (g_dragPanel) {
        RECT r = g_dragRect;
#ifdef LP_BUG_DRAG
        /* 对照组：用 lParam 的客户区坐标当基准（旧写法，会抽搐）。 */
        const int dx = px - g_dragOriginScreen.x, dy = py - g_dragOriginScreen.y;
#else
        const int dx = sx - g_dragOriginScreen.x, dy = sy - g_dragOriginScreen.y;
#endif
        /* SWP_NOZORDER：窗口已经是 topmost，没必要每条消息都重申一次层级
           （重申会触发一次 z 序更新，拖动时每秒上千次纯属浪费）。 */
        SetWindowPos(h, nullptr, r.left + dx, r.top + dy, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      if (g_dragItem >= 0) {
        applySliderFromX((*g_items)[g_dragItem], (float)px);
        renderThrottled();
        return 0;
      }
      if (g_openSel >= 0) {
        const int oi = hitOption(px, py);
        if (oi != g_hoverOpt) {
          g_hoverOpt = oi;
          renderThrottled();
        }
        return 0;
      }
      const int idx = hitItem(px, py, nullptr);
      if (idx != g_hoverItem) {
        g_hoverItem = idx;
        renderThrottled();
      }
      /* 悬停在可操作项上时换成手型，暗示可点/可拖 */
      if (!g_curHand) {
        g_curHand = LoadCursorW(nullptr, IDC_HAND);
        g_curArrow = LoadCursorW(nullptr, IDC_ARROW);
      }
      SetCursor(idx >= 0 ? g_curHand : g_curArrow);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (g_dragPanel) {
        g_dragPanel = false;
        ReleaseCapture();
        return 0;
      }
      if (g_dragItem >= 0) {
        g_dragItem = -1;
        ReleaseCapture();
        render();
        return 0;
      }
      return 0;
    }
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(w);
      if (g_openSel >= 0) {
        const LpPanelItem& it = (*g_items)[g_openSel];
        const float rowH = 28.f * g_scale;
        const float lh = (float)g_selListRect.bottom - g_selListRect.top;
        const float maxScroll = maxf(0.f, (float)it.options.size() * rowH + 8.f - lh);
        g_selScroll = clampf(g_selScroll - (float)delta / 120.f * rowH * 1.5f, 0.f, maxScroll);
      } else if (g_scrollMax > 0.5f) {
        g_scroll = clampf(g_scroll - (float)delta / 120.f * 60.f * g_scale, 0.f, g_scrollMax);
      } else {
        return 0;
      }
      render();
      return 0;
    }
    case WM_KEYDOWN:
      if (w == VK_ESCAPE) {
        if (g_openSel >= 0) {
          g_openSel = -1;
          render();
        } else {
          lp_panel_hide();
        }
        return 0;
      }
      return 0;
    case WM_CLOSE:
      lp_panel_hide();
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE;
    case WM_ACTIVATE:
      if (LOWORD(w) == WA_INACTIVE && g_openSel >= 0) {
        g_openSel = -1;
        render();
      }
      return 0;
    default:
      return DefWindowProcW(h, m, w, l);
  }
}

/* ============================ 对外接口 ============================ */

bool lp_panel_open(HINSTANCE hInst, const std::wstring& title,
                   std::vector<LpPanelItem>* items, LpPanelNotify notify, void* user) {
  if (g_hwnd) return true;
  g_items = items;
  g_notify = notify;
  g_notifyUser = user;
  g_title = title;
  g_scale = detectScale();

  GdiplusStartupInput gsi;
  if (GdiplusStartup(&g_gdiToken, &gsi, nullptr) != Ok) return false;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = panelProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kPanelClass;
  RegisterClassExW(&wc);

  /* ★ 面板**必须能点**：所以只带 WS_EX_LAYERED（给 UpdateLayeredWindow 用），
     绝不能带 WS_EX_TRANSPARENT —— 那是覆盖层穿透用的，加上去面板就点不动了。 */
  const DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

  g_winW = (int)std::ceil(320.f * g_scale);
  g_winH = (int)std::ceil(300.f * g_scale);

  /* 初始位置：主屏左下角，跟浏览器版一样 */
  const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  const int x = (int)(18.f * g_scale);
  int y = sh - g_winH - (int)(18.f * g_scale);
  if (y < 0) y = 0;

  g_hwnd = CreateWindowExW(ex, kPanelClass, L"光柱 · 调节面板", WS_POPUP,
                           x, y, g_winW, g_winH, nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) {
    GdiplusShutdown(g_gdiToken);
    g_gdiToken = 0;
    return false;
  }

  if (!ensureBuffer(g_winW, g_winH) || !ensureFonts()) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    GdiplusShutdown(g_gdiToken);
    g_gdiToken = 0;
    return false;
  }
  (void)sw;
  resizeWindowToContent();
  return true;
}

void lp_panel_close() {
  if (g_hwnd) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  destroyBuffer();
  delete g_fSmall; g_fSmall = nullptr;
  delete g_fBody; g_fBody = nullptr;
  delete g_fBodyB; g_fBodyB = nullptr;
  delete g_fSec; g_fSec = nullptr;
  delete g_fTab; g_fTab = nullptr;
  delete g_fTabB; g_fTabB = nullptr;
  delete g_fVal; g_fVal = nullptr;
  delete g_fTitle; g_fTitle = nullptr;
  if (g_family) { delete g_family; g_family = nullptr; }
  if (g_gdiToken) {
    GdiplusShutdown(g_gdiToken);
    g_gdiToken = 0;
  }
  g_items = nullptr;
  g_openSel = -1;
  g_dragItem = -1;
  g_hoverItem = -1;
}

bool lp_panel_is_open() { return g_hwnd && IsWindowVisible(g_hwnd); }
HWND lp_panel_hwnd() { return g_hwnd; }

void lp_panel_show() {
  if (!g_hwnd) return;
  resizeWindowToContent();
  render();
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void lp_panel_hide() {
  if (!g_hwnd) return;
  g_openSel = -1;
  g_dragItem = -1;
  g_dragPanel = false;
  ShowWindow(g_hwnd, SW_HIDE);
}

void lp_panel_toggle() {
  if (lp_panel_is_open()) lp_panel_hide();
  else lp_panel_show();
}

void lp_panel_sync() {
  if (!g_hwnd) return;
  resizeWindowToContent();
  render();
}

void lp_panel_set_tab(int tab) {
  g_tab = (tab == 1) ? 1 : 0;
  g_scroll = 0.f;
  resizeWindowToContent();
  render();
}

void lp_panel_set_meter(float v01) {
  g_meter = clampf(v01, 0.f, 1.f);
  if (lp_panel_is_open()) render();
}

void lp_panel_set_status(const std::wstring& s) {
  if (s == g_status) return;
  const bool had = !g_status.empty();
  g_status = s;
  const bool has = !g_status.empty();
  if (has != had) resizeWindowToContent();   // 状态条出现/消失 → 窗口高度要跟着变
  if (lp_panel_is_open()) render();
}

int lp_panel_item_count() { return g_items ? (int)g_items->size() : 0; }

int lp_panel_slider_count() {
  int n = 0;
  if (g_items)
    for (auto& it : *g_items)
      if (it.kind == LP_SLIDER) n++;
  return n;
}

void lp_panel_reset_all() {
  if (!g_items) return;
  for (auto& it : *g_items) {
    if (it.kind == LP_SLIDER && it.fval) *it.fval = it.deflt;
    else if (it.kind == LP_SWITCH && it.bval) *it.bval = it.deflt != 0.f;
    else if ((it.kind == LP_SELECT || it.kind == LP_SEGMENT) && it.ival)
      *it.ival = (int)(it.deflt + 0.5f);
  }
  lp_panel_sync();
}

bool lp_panel_capture(std::vector<unsigned char>* out, int* w, int* h) {
  if (!g_hwnd || !g_bits || g_bufW <= 0 || g_bufH <= 0) return false;
  render();
  out->resize((size_t)g_bufW * (size_t)g_bufH * 4);
  const unsigned char* src = (const unsigned char*)g_bits;
  /* 反预乘：ULW 要的是预乘值，写给人看的图要直值 */
  for (size_t i = 0; i < (size_t)g_bufW * (size_t)g_bufH; i++) {
    const unsigned char a = src[i * 4 + 3];
    if (a == 0) {
      out->at(i * 4 + 0) = out->at(i * 4 + 1) = out->at(i * 4 + 2) = 0;
      out->at(i * 4 + 3) = 0;
    } else {
      for (int c = 0; c < 3; c++) {
        unsigned v = (unsigned)src[i * 4 + c] * 255u / a;
        out->at(i * 4 + c) = (unsigned char)(v > 255 ? 255 : v);
      }
      out->at(i * 4 + 3) = a;
    }
  }
  if (w) *w = g_bufW;
  if (h) *h = g_bufH;
  return true;
}

bool lp_panel_test_drag(int id, float frac) {
  if (!g_items) return false;
  for (auto& it : *g_items) {
    if (it.id != id || it.kind != LP_SLIDER) continue;
    const float before = it.fval ? *it.fval : 0.f;
    applySliderFromX(it, it.trX + clampf(frac, 0.f, 1.f) * it.trW);
    lp_panel_sync();
    return it.fval && *it.fval != before;
  }
  return false;
}

bool lp_panel_test_pick(int id, int index) {
  if (!g_items) return false;
  for (auto& it : *g_items) {
    if (it.id != id || (it.kind != LP_SELECT && it.kind != LP_SEGMENT)) continue;
    if (!it.ival || index < 0 || index >= (int)it.options.size()) return false;
    const int before = *it.ival;
    *it.ival = index;
    notifyChanged(it.id);
    lp_panel_sync();
    return *it.ival != before;
  }
  return false;
}

int lp_panel_test_hit(int px, int py) {
  if (!g_hwnd) return -1;
  return hitItem(px, py, nullptr);
}
