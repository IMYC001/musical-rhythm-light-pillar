#include "d3d.h"

#include <d3dcompiler.h>
/* IDXGISwapChain2 / SetMaximumFrameLatency 在 dxgi1_3.h 里
   （⚠ 不是 dxgi1_2.h —— 那个头里只有 IDXGIDevice2）。
   d3d11.h 只带 dxgi.h，不带 1_3。 */
#include <dxgi1_3.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstring>

/* MinGW 的 winuser.h 不一定带这个常量（Win8 才引入）。
   它表示「这个窗口没有 GDI 重定向表面，内容全部由 DirectComposition 提供」，
   是透明覆盖层的关键。 */
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static const wchar_t* LP_OVERLAY_CLASS = L"LpOverlayWindowD3D";

/* 交换链配置变体（诊断用，见 lp_d3d_create 里的注释）。正常运行恒为 0。 */
static int g_swapVariant = 0;
void lp_d3d_set_swap_variant(int v) { g_swapVariant = v; }

/* 诊断用：Present 之后调 DwmFlush()。
   思路：我们用的是 SyncInterval=0，提交节奏和 DWM 的合成节拍是**自由跑**的，
   长期会漂移。如果那个「每 ~4.86 秒一次、约 400ms 的 Present 阻塞」是
   相位漂移到某个临界点后的补偿行为，那么每帧主动和 DWM 对一次齐
   （DwmFlush 会一直等到下一次合成）就能消掉它。
   代价是帧率被 DWM 的合成节拍接管，但本来就是 60Hz 屏、我们也是 60fps。 */
static bool g_dwmFlush = false;
void lp_d3d_set_dwm_flush(bool on) { g_dwmFlush = on; }

/* ============================ 窗口过程 ============================ */

static LRESULT CALLBACK lpOverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    /* ④ 鼠标穿透。
     *
     *  这里**故意不处理 WM_NCHITTEST**（早期版本回 HTTRANSPARENT，是错的）。
     *
     *  穿透完全交给窗口样式对：WS_EX_LAYERED | WS_EX_TRANSPARENT
     *  （见 lp_d3d_create）。系统在**命中测试阶段**就把这个窗口整个跳过，
     *  跨进程照样生效 —— 实测覆盖层连 WM_NCHITTEST 都收不到。
     *
     *  ⚠ 为什么不能用 WM_NCHITTEST 回 HTTRANSPARENT 兜底：
     *    文档写的是「消息会传给**同一线程**里被覆盖的底层窗口」。
     *    我们的覆盖层是置顶全屏，下面全是**别的进程**（桌面 / 任务栏 /
     *    其他程序）→ 跨进程时这个点击会被**直接丢弃**。
     *    实测：覆盖层和靶窗口**都**收不到点击 —— 也就是"点击凭空消失"，
     *    比穿透失败还糟（用户看到的就是"鼠标点不动任何东西"）。
     */
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      EndPaint(h, &ps);
      return 0;
    }
    /* 覆盖层不允许被关掉，退出只走托盘 / 热键 */
    case WM_CLOSE:
      return 0;
    case WM_NCACTIVATE:
      return 1;
    case WM_ACTIVATE:
      return 0;
    default:
      return DefWindowProcW(h, m, w, l);
  }
}

static bool ensureClass(HINSTANCE hInst) {
  static bool done = false;
  if (done) return true;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = 0;
  wc.lpfnWndProc = lpOverlayProc;
  wc.hInstance = hInst;
  wc.hCursor = nullptr;
  wc.hbrBackground = nullptr;
  wc.lpszClassName = LP_OVERLAY_CLASS;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  done = true;
  return true;
}

/* ============================ 创建 ============================ */

bool lp_d3d_create(HINSTANCE hInst, const RECT& rect, LpD3DWindow* out, std::string* err) {
  if (!ensureClass(hInst)) {
    if (err) *err = "注册窗口类失败";
    return false;
  }

  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;

  /* ① 无重定向表面 + 置顶 + 不激活 + 不进 alt-tab + ★鼠标穿透
   *
   *  ★ 穿透靠的是 **WS_EX_LAYERED | WS_EX_TRANSPARENT 这一对，缺一不可**。
   *    实测矩阵（src/hitlab.cpp，跨进程真点，原始数据在 verify3/）：
   *        TRANSPARENT 单独（非分层窗口）→ 失败（照样收到 WM_NCHITTEST + 点击）
   *        LAYERED 单独                  → 失败
   *        LAYERED | TRANSPARENT         → **成功**
   *    也就是说 WS_EX_TRANSPARENT 只对**分层窗口**起穿透作用。官方文档对它
   *    只写了绘制语义（"同级姐妹绘制前不绘制本窗口"），压根没提命中测试 ——
   *    所以不能靠文档推，只能实测。
   *
   *    另外还得让窗口**真的是分层窗口**：调一次 SetLayeredWindowAttributes。
   *    这里给 LWA_ALPHA=255（全不透明），理由：
   *      · 本窗口有 WS_EX_NOREDIRECTIONBITMAP，没有重定向表面，
   *        LWA 对渲染不产生任何影响 —— DComp 的内容照样显示；
   *      · 它只是把窗口标记成"真分层"，穿透才会生效。
   *    实测 LWA_ALPHA=255 + NOREDIR|LAYERED|TRANSPARENT → 穿透成功，
   *    而且**画面完全不受影响**（保真帧 最大RGB=98 不变）。
   *
   *  ⚠ 千万别再用 WM_NCHITTEST 回 HTTRANSPARENT 那条路（v1/v2 都栽在这）：
   *    文档明确限定它只对**同一线程**里被覆盖的底层窗口生效，跨进程时
   *    点击会被**直接丢弃** —— 实测覆盖层和靶窗口都收不到点击，
   *    表现为"点击凭空消失"，比穿透失败还糟。
   */
  DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                  WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT;
  /* 诊断变体 6：去掉 WS_EX_LAYERED。
     怀疑点：这个窗口同时带 WS_EX_NOREDIRECTIONBITMAP（没有重定向表面）
     和 WS_EX_LAYERED（分层窗口），系统要给一个「没有表面的分层窗口」
     维护分层属性，可能触发周期性的重组 → Present 阻塞几百毫秒。
     去掉之后穿透会失效，只用来量帧节奏。 */
  const bool noLayered = (g_swapVariant == 6);
  if (noLayered) exStyle &= ~(DWORD)WS_EX_LAYERED;

  HWND hwnd = CreateWindowExW(exStyle, LP_OVERLAY_CLASS, L"光柱", WS_POPUP,
                              rect.left, rect.top, w, h, nullptr, nullptr, hInst, nullptr);
  if (!hwnd) {
    if (err) *err = "CreateWindowExW 失败";
    return false;
  }

  /* 让窗口成为"真正的分层窗口" —— 没有这一步，WS_EX_TRANSPARENT 不产生穿透。 */
  if (!noLayered) SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

  /* ---- D3D11 设备 ---- */
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 4,
                                 D3D11_SDK_VERSION, &device, &got, &ctx);
  if (FAILED(hr)) {
    /* 老系统上没有 11_1 运行时，去掉它重试 */
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 3,
                           D3D11_SDK_VERSION, &device, &got, &ctx);
  }
  if (FAILED(hr) || !device) {
    if (err) *err = "D3D11CreateDevice 失败（没有可用的硬件设备）";
    DestroyWindow(hwnd);
    return false;
  }

  /* ---- DXGI 工厂 ---- */
  IDXGIDevice* dxgiDev = nullptr;
  device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
  if (!dxgiDev) {
    if (err) *err = "拿不到 IDXGIDevice";
    ctx->Release(); device->Release(); DestroyWindow(hwnd);
    return false;
  }
  IDXGIAdapter* adapter = nullptr;
  dxgiDev->GetAdapter(&adapter);
  IDXGIFactory2* factory = nullptr;
  if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
  if (!factory) {
    if (err) *err = "拿不到 IDXGIFactory2（需要 Windows 8 及以上）";
    if (adapter) adapter->Release();
    dxgiDev->Release(); ctx->Release(); device->Release(); DestroyWindow(hwnd);
    return false;
  }

  /* ---- ② 预乘 alpha 的合成交换链 ---- */
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = (UINT)w;
  sd.Height = (UINT)h;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.Stereo = FALSE;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  /* BufferCount 给足。
   * BufferCount=2（第一版）时只剩 1 帧余量，一旦 DWM 那一拍来晚一点，
   * Present 就得等一个后台缓冲空出来。给足缓冲成本几乎为零，所以给 8。
   * ⚠ 但别指望它解决长帧：实测 2 / 3 / 8 / 16 都会出现
   *   「每 ~4.86 秒一次、约 400ms」的阻塞（详见下面 Present 处的长注释），
   *   说明那个停顿不是缓冲耗尽造成的。 */
  sd.BufferCount = 8;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  sd.Flags = 0;

  /* 诊断用变体开关（只影响本函数，正常运行恒为 0）：
   *   0 = 默认（FLIP_SEQUENTIAL / BufferCount 8）
   *   1 = BufferCount 16
   *   2 = 加 FRAME_LATENCY_WAITABLE_OBJECT + SetMaximumFrameLatency(1)
   *   3 = FLIP_DISCARD
   *   5 = CreateTargetForHwnd 的 topmost 传 FALSE
   *   6 = 去掉 WS_EX_LAYERED
   * 都是「Present 偶发阻塞」的候选解法，逐个实测过 —— **全部无效**。
   * ⚠ 别再试 DXGI_SCALING_NONE：合成交换链不支持，创建会失败，
   *   然后卡在错误对话框上（实测挂死到超时）。 */
  if (g_swapVariant == 1) {
    sd.BufferCount = 16;
  } else if (g_swapVariant == 2) {
    sd.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  } else if (g_swapVariant == 3) {
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  }

  IDXGISwapChain1* swap = nullptr;
  hr = factory->CreateSwapChainForComposition(device, &sd, nullptr, &swap);
  if (FAILED(hr) || !swap) {
    if (err) *err = "CreateSwapChainForComposition 失败（HRESULT 0x" +
                    std::to_string((unsigned long)hr) + "）";
    factory->Release(); adapter->Release(); dxgiDev->Release();
    ctx->Release(); device->Release(); DestroyWindow(hwnd);
    return false;
  }

  /* 变体 2：把在飞的帧数压到 1，让 Present 有确定的同步点 */
  if (g_swapVariant == 2) {
    IDXGISwapChain2* swap2 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&swap2)) && swap2) {
      swap2->SetMaximumFrameLatency(1);
      swap2->Release();
    }
  }

  /* ---- 渲染目标 ---- */
  ID3D11Texture2D* back = nullptr;
  swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
  ID3D11RenderTargetView* rtv = nullptr;
  if (back) {
    device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
  }
  if (!rtv) {
    if (err) *err = "创建渲染目标视图失败";
    swap->Release(); factory->Release(); adapter->Release(); dxgiDev->Release();
    ctx->Release(); device->Release(); DestroyWindow(hwnd);
    return false;
  }

  /* ---- ③ DirectComposition ---- */
  IDCompositionDevice* dcomp = nullptr;
  hr = DCompositionCreateDevice(dxgiDev, __uuidof(IDCompositionDevice), (void**)&dcomp);
  if (FAILED(hr) || !dcomp) {
    if (err) *err = "DCompositionCreateDevice 失败";
    rtv->Release(); swap->Release(); factory->Release(); adapter->Release();
    dxgiDev->Release(); ctx->Release(); device->Release(); DestroyWindow(hwnd);
    return false;
  }
  IDCompositionTarget* target = nullptr;
  IDCompositionVisual* visual = nullptr;
  /* 第二个参数 = 「这个 target 在窗口内是否置顶」。
     变体 5 传 FALSE，用来验证「每 ~4.86 秒一次的 Present 阻塞」是不是
     DWM 在反复重排这个 topmost target 造成的。 */
  hr = dcomp->CreateTargetForHwnd(hwnd, g_swapVariant == 5 ? FALSE : TRUE, &target);
  if (FAILED(hr) || !target) {
    if (err) *err = "CreateTargetForHwnd 失败 hr=0x" + std::to_string((unsigned long)hr);
    dcomp->Release(); rtv->Release(); swap->Release(); factory->Release();
    adapter->Release(); dxgiDev->Release(); ctx->Release(); device->Release();
    DestroyWindow(hwnd);
    return false;
  }
  hr = dcomp->CreateVisual(&visual);
  if (FAILED(hr) || !visual) {
    if (err) *err = "CreateVisual 失败 hr=0x" + std::to_string((unsigned long)hr);
    target->Release(); dcomp->Release(); rtv->Release(); swap->Release();
    factory->Release(); adapter->Release(); dxgiDev->Release(); ctx->Release();
    device->Release(); DestroyWindow(hwnd);
    return false;
  }
  hr = visual->SetContent(swap);
  if (FAILED(hr)) {
    if (err) *err = "visual->SetContent 失败 hr=0x" + std::to_string((unsigned long)hr);
    visual->Release(); target->Release(); dcomp->Release(); rtv->Release();
    swap->Release(); factory->Release(); adapter->Release(); dxgiDev->Release();
    ctx->Release(); device->Release(); DestroyWindow(hwnd);
    return false;
  }
  hr = target->SetRoot(visual);
  if (FAILED(hr)) {
    if (err) *err = "target->SetRoot 失败 hr=0x" + std::to_string((unsigned long)hr);
    visual->Release(); target->Release(); dcomp->Release(); rtv->Release();
    swap->Release(); factory->Release(); adapter->Release(); dxgiDev->Release();
    ctx->Release(); device->Release(); DestroyWindow(hwnd);
    return false;
  }
  hr = dcomp->Commit();
  if (FAILED(hr)) {
    if (err) *err = "dcomp->Commit 失败 hr=0x" + std::to_string((unsigned long)hr);
    visual->Release(); target->Release(); dcomp->Release(); rtv->Release();
    swap->Release(); factory->Release(); adapter->Release(); dxgiDev->Release();
    ctx->Release(); device->Release(); DestroyWindow(hwnd);
    return false;
  }

  /* ---- 常量缓冲（144 字节 = 9 × float4）---- */
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(LpParams);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ID3D11Buffer* cb = nullptr;
  device->CreateBuffer(&bd, nullptr, &cb);

  /* ---- ★ 光栅化状态：必须关掉背面剔除 ----
     D3D11 的默认光栅化状态是 CullMode = D3D11_CULL_BACK，
     而我们的全屏四边形在屏幕空间是**逆时针**绕序（顶点 (0,0),(2,0),(0,2),(2,2)
     映射到 NDC 后算出的有向面积为负）→ 会被整块剔掉。
     症状极具迷惑性：窗口可见、Present 返回 S_OK、帧率正常，
     但屏幕上只有清屏色（alpha 0.004 ≈ 全透明），看起来「什么都没发生」。
     OpenGL 默认不做面剔除，所以 GLSL 版没有这个坑。 */
  ID3D11RasterizerState* rast = nullptr;
  {
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    rd.MultisampleEnable = FALSE;
    device->CreateRasterizerState(&rd, &rast);
  }

  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  factory->Release();
  adapter->Release();
  dxgiDev->Release();

  out->hwnd = hwnd;
  out->rect = rect;
  out->w = w;
  out->h = h;
  out->device = device;
  out->ctx = ctx;
  out->swap = swap;
  out->rtv = rtv;
  out->dcomp = dcomp;
  out->target = target;
  out->visual = visual;
  out->cb = cb;
  out->rast = rast;
  out->valid = true;

  /* 显示但不激活：绝不抢焦点 */
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd, HWND_TOPMOST, rect.left, rect.top, w, h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  return true;
}

/* ============================ 着色器 ============================ */

static bool compileStage(const char* src, const char* entry, const char* target,
                         ID3DBlob** blob, std::string* err) {
  ID3DBlob* errBlob = nullptr;
  HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errBlob);
  if (FAILED(hr)) {
    if (err) {
      *err = std::string("HLSL 编译失败 (") + entry + "):\n";
      if (errBlob) *err += (const char*)errBlob->GetBufferPointer();
    }
    if (errBlob) errBlob->Release();
    return false;
  }
  if (errBlob) errBlob->Release();
  return true;
}

bool lp_d3d_set_shaders(LpD3DWindow* w, const char* hlslVs, const char* hlslFs,
                        std::string* err) {
  if (!w->valid) return false;

  ID3DBlob* vsb = nullptr;
  ID3DBlob* psb = nullptr;
  if (!compileStage(hlslVs, "VSMain", "vs_4_0", &vsb, err)) return false;
  if (!compileStage(hlslFs, "PSMain", "ps_4_0", &psb, err)) {
    vsb->Release();
    return false;
  }

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  HRESULT hr1 = w->device->CreateVertexShader(vsb->GetBufferPointer(),
                                              vsb->GetBufferSize(), nullptr, &vs);
  HRESULT hr2 = w->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                             nullptr, &ps);
  vsb->Release();
  psb->Release();
  if (FAILED(hr1) || FAILED(hr2) || !vs || !ps) {
    if (err) *err = "CreateVertexShader / CreatePixelShader 失败";
    if (vs) vs->Release();
    if (ps) ps->Release();
    return false;
  }

  if (w->vs) w->vs->Release();
  if (w->ps) w->ps->Release();
  w->vs = vs;
  w->ps = ps;
  return true;
}

/* ============================ 渲染 ============================ */

bool lp_d3d_render(LpD3DWindow* w, const LpParams& p) {
  if (!w->valid || !w->vs || !w->ps || !w->cb) return false;

  LARGE_INTEGER f, a, b, c;
  QueryPerformanceFrequency(&f);
  auto ms = [&](const LARGE_INTEGER& x, const LARGE_INTEGER& y) {
    return (double)(y.QuadPart - x.QuadPart) * 1000.0 / (double)f.QuadPart;
  };

  QueryPerformanceCounter(&a);
  D3D11_MAPPED_SUBRESOURCE msr = {};
  if (SUCCEEDED(w->ctx->Map(w->cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &msr))) {
    memcpy(msr.pData, &p, sizeof(LpParams));
    w->ctx->Unmap(w->cb, 0);
  }

  /* 清成全透明黑。alpha 给 0.004 而不是 0：整层 alpha=0 时
     某些合成路径会把「完全透明的图层」整层丢掉。 */
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.004f};
  w->ctx->ClearRenderTargetView(w->rtv, clear);

  ID3D11RenderTargetView* rtvs[1] = {w->rtv};
  w->ctx->OMSetRenderTargets(1, rtvs, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)w->w;
  vp.Height = (float)w->h;
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  w->ctx->RSSetViewports(1, &vp);

  w->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  w->ctx->IASetInputLayout(nullptr);
  w->ctx->RSSetState(w->rast);   // ★ 必须绑：默认状态会剔掉我们的四边形
  w->ctx->VSSetShader(w->vs, nullptr, 0);
  w->ctx->PSSetShader(w->ps, nullptr, 0);
  ID3D11Buffer* cbs[1] = {w->cb};
  w->ctx->PSSetConstantBuffers(0, 1, cbs);
  w->ctx->Draw(4, 0);
  QueryPerformanceCounter(&b);
  w->lastMapMs = ms(a, b);   // Map + 清屏 + 绘制（都在 GPU 命令队列里，正常极快）

  /* 诊断回读：必须在 Present 之前 —— flip 模型的交换链在 Present 之后
     GetBuffer(0) 拿到的是**下一个**后台缓冲（内容未定义）。 */
  if (w->wantReadback) {
    if (!w->staging) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = (UINT)w->w;
      td.Height = (UINT)w->h;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_STAGING;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      w->device->CreateTexture2D(&td, nullptr, &w->staging);
    }
    ID3D11Texture2D* back = nullptr;
    w->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (back && w->staging) {
      w->ctx->CopyResource(w->staging, back);
      D3D11_MAPPED_SUBRESOURCE rm = {};
      if (SUCCEEDED(w->ctx->Map(w->staging, 0, D3D11_MAP_READ, 0, &rm))) {
        w->lastFrame.resize((size_t)w->w * (size_t)w->h * 4);
        for (int y = 0; y < w->h; y++) {
          memcpy(&w->lastFrame[(size_t)y * (size_t)w->w * 4],
                 (const unsigned char*)rm.pData + (size_t)y * rm.RowPitch,
                 (size_t)w->w * 4);
        }
        w->ctx->Unmap(w->staging, 0);
      }
    }
    if (back) back->Release();
  }

  /* 提交。
   *
   *  ⚠ SyncInterval 用 0，**不是** 1。
   *    合成交换链（CreateSwapChainForComposition）的内容交给 DWM 去合成，
   *    最终上屏由 DWM 按显示器刷新率做 —— SyncInterval 对画面**没有影响**
   *    （不会撕裂，也不会同步到垂直回扫），它唯一的作用是「Present 要不要等」。
   *    传 1 的语义在合成交换链上会退化成「等 DWM 把后台缓冲还回来」。
   *    帧率我们自己用高精度定时器控（见 main.cpp），所以这里要立即返回。
   *
   *  ── 关于「每 ~4.86 秒一次、约 400ms 的 Present 阻塞」（已彻底查清，未解）──
   *
   *  症状：帧间隔 p50 16.96 / p95 17.47 / p99 17.6 ms（≈59fps）都非常好，
   *        但「最长帧」每 ~4.86 秒就蹦到 400ms 上下 —— p50/p95 完全看不出来，
   *        用户感受就是「隔几秒顿一下，看着卡卡的」。分阶段计时把它钉死在
   *        Present 里（最慢那一帧 Map+绘制 0.011ms、Present 400ms），
   *        而 Present 每次都返回 S_OK（没有 OCCLUDED、没有设备丢失）。
   *
   *  已经逐一实测排除的（**全部无效**，周期恒为 ~4.86 秒）：
   *    · BufferCount 2 / 3 / 8 / 16
   *    · SwapEffect FLIP_SEQUENTIAL / FLIP_DISCARD
   *    · SyncInterval 1
   *    · DXGI_PRESENT_DO_NOT_WAIT（一次都没触发 WAS_STILL_DRAWING）
   *    · FRAME_LATENCY_WAITABLE_OBJECT + SetMaximumFrameLatency(1)
   *    · 每帧 DwmFlush() 与 DWM 合成节拍对齐
   *    · 每帧 dcomp->Commit()
   *    · CreateTargetForHwnd 的 topmost 参数 TRUE/FALSE
   *    · 窗口四边内缩 120px（排除全屏检测 / 硬件平面 MPO）
   *    · 去掉 WS_EX_LAYERED（排除「无重定向表面的分层窗口」）
   *    · timeBeginPeriod(1) 开 / 关
   *    · 音频输入开 / 关
   *    · fpsLimit 30 / 60 / 120 / 144（**周期与帧率无关**，纯墙钟）
   *
   *  关键对照实验（这才是定性依据）：
   *    ① 加 --nopresent（只跳过 Present，其余照跑）：
   *       渲染耗时从均值 1.68ms / 最长 417ms 掉到均值 0.010ms / 最长 0.101ms，
   *       一次长帧都没有 → 阻塞确实只在 Present 里。
   *    ② 另起一个**不含任何 D3D** 的纯计时探针（src/gapwatch.cpp）并行跑：
   *       探针 0 个长间隙（最长 17.67ms），主程序同期 4 次 424ms
   *       → CPU 调度完全健康，不是被系统抢占，是 GPU 呈现路径在停。
   *
   *  结论：这是**这台机器上（AMD 驱动 / 1600x900@144Hz）D3D 呈现路径的
   *        周期性停顿**，应用侧任何交换链配置都绕不开。留 DO_NOT_WAIT 是
   *        作为「缓冲真的耗尽时」的兜底（在其他机器上可能有意义），
   *        它对本机的这个停顿无效。诊断开关见 main.cpp 的 --swap/--nopresent。 */
  HRESULT hr;
  if (w->skipPresent) {
    /* 诊断模式：连 Present 都不调。如果长帧**依然**出现，说明它不是
       Present 在等，而是我们的线程在这几行被系统抢占了 —— 那再怎么调
       交换链参数都没用，得换思路。实测：跳过之后一次长帧都没有。 */
    w->lastPresentHr = 0;
    return true;
  }
  if (w->presentDoNotWait) {
    hr = w->swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (hr == DXGI_ERROR_INVALID_CALL) {
      /* 少见：这个交换链不接受 DO_NOT_WAIT。退回普通 Present，别死磕。 */
      w->presentDoNotWait = false;
      hr = w->swap->Present(0, 0);
    } else if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      w->presentSkipped++;
      QueryPerformanceCounter(&c);
      w->lastPresentMs = ms(b, c);
      w->lastPresentHr = (long)hr;
      return true;               // 不是失败，只是这一帧没提交
    }
  } else {
    hr = w->swap->Present(0, 0);
  }
  QueryPerformanceCounter(&c);
  w->lastPresentMs = ms(b, c);
  w->lastPresentHr = (long)hr;
  /* 返回值统计。DXGI_STATUS_OCCLUDED（0x087A0001）是个**成功**码，
     但语义是「窗口被判为被遮住，内容不会上屏」—— 这时候 Present 会
     一直阻塞到窗口重新可见。周期性出现它就能解释规律性长帧。 */
  if (hr == DXGI_STATUS_OCCLUDED) {
    w->presentOccluded++;
    w->presentHrLastBad = (long)hr;
  } else if (hr != S_OK) {
    w->presentHrOther++;
    w->presentHrLastBad = (long)hr;
  }
  /* 诊断：主动与 DWM 的合成节拍对齐一次 */
  if (g_dwmFlush) DwmFlush();
  /* 变体 4：每帧显式 Commit 一次 DComp 事务 */
  if (g_swapVariant == 4 && w->dcomp) w->dcomp->Commit();
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return false;
  return true;
}

/* ============================ 销毁 ============================ */

void lp_d3d_destroy(LpD3DWindow* w) {
  if (!w) return;
  if (w->staging) w->staging->Release();
  if (w->rast) w->rast->Release();
  if (w->cb) w->cb->Release();
  if (w->vs) w->vs->Release();
  if (w->ps) w->ps->Release();
  if (w->target) w->target->Release();
  if (w->visual) w->visual->Release();
  if (w->dcomp) w->dcomp->Release();
  if (w->rtv) w->rtv->Release();
  if (w->swap) w->swap->Release();
  if (w->ctx) w->ctx->Release();
  if (w->device) w->device->Release();
  if (w->hwnd) DestroyWindow(w->hwnd);
  *w = LpD3DWindow{};
}

void lp_reassert_topmost(LpD3DWindow* w) {
  if (!w || !w->valid) return;
  SetWindowPos(w->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* ============================ 显示器枚举 ============================ */

struct LpEnumCtx {
  std::vector<LpMonitor>* out;
};

static BOOL CALLBACK lpMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM data) {
  auto* ctx = reinterpret_cast<LpEnumCtx*>(data);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  LpMonitor m;
  m.rect = mi.rcMonitor;  // 整块物理屏，不是 rcWork
  m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  ctx->out->push_back(m);
  return TRUE;
}

void lp_enum_monitors(std::vector<LpMonitor>* out) {
  out->clear();
  LpEnumCtx ctx{out};
  EnumDisplayMonitors(nullptr, nullptr, lpMonitorProc, reinterpret_cast<LPARAM>(&ctx));
  for (size_t i = 0; i < out->size(); i++) {
    if ((*out)[i].primary && i != 0) {
      std::swap((*out)[0], (*out)[i]);
      break;
    }
  }
}
