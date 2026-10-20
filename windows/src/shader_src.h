/*
 * 着色器源码（HLSL SM 4.0，运行时用 d3dcompiler_47 编译）。
 *
 * 移植自作品的 _src/vertex.glsl 与 _src/fragment.glsl，**造型与驱动数学一行没动**，
 * 只做了必要的语言转换：
 *
 *   vec2/3/4 → float2/3/4    mix → lerp     gl_FragColor → 返回值
 *   varying/uniform → 语义 + cbuffer
 *   语言从 GLSL 换成 HLSL 的原因：这台机器**没有硬件 OpenGL 驱动**
 *   （诊断实测 glGetString(GL_VERSION) = "1.1.0"、GL_RENDERER = "GDI Generic"、
 *     wglGetProcAddress 全部返回 null），OpenGL 只能到 1.1 软件渲染，没有着色器。
 *   而 D3D11 是好的（Electron 的 WebGL 本来就是 ANGLE→D3D11）。
 *
 * ⚠ 除语言转换外，光柱的距离场、软饱和、两种风格的分支、空闲呼吸、
 *   强调色染色顺序，全部逐字保留 —— 包括那几处「看起来多余」的写法和注释里
 *   标了 ⚠ 的坑（drive 必须在分支外算、halo 必须用 pow、breathe 必须是加法叠加）。
 *
 * ⚠ cbuffer 的布局：**故意全部用 float4 打包**，不用裸 float/float2。
 *   HLSL 的 cbuffer 打包规则是「变量不能跨 16 字节边界」，裸 float2 落在
 *   offset 12 时 HLSL 会挪到 16，而 C++ 结构体不会 —— 这是最经典的一类
 *   静默错位（uniform 值全对、画面就是不对）。全用 float4 就没有这个自由度，
 *   C++ 侧 memcpy 进去必然对齐。下面再把每个分量解包成与原作品同名的局部量，
 *   于是正文一行都不用改。
 */
#pragma once

static const char* LP_HLSL_VS = R"HLSL(
struct VSOut {
  float4 pos : SV_POSITION;
  float2 uv  : TEXCOORD0;
};

// 全屏四边形直接用 SV_VertexID 生成，不需要顶点缓冲。
// id=0..3 → p=(0,0),(2,0),(0,2),(2,2)；p 在 0..2 上，映射到 NDC -1..3，
// 可见区间 NDC[-1,1] 恰好对应 p∈[0,1]，所以 uv = p 在屏幕上正好是 0..1。
// 注意 D3D 的 NDC 是 y 向上，和原 GLSL 版一致，uv.y=0 在下边。
VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  float2 p = float2(float((id << 1) & 2), float(id & 2));
  o.uv = p;
  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
  return o;
}
)HLSL";

static const char* LP_HLSL_FS = R"HLSL(
cbuffer LpParams : register(b0) {
  float4 P0;   // xy = uRes,          z = uStyleMode,   w = uBeamInset
  float4 P1;   // x  = uAudioIntensity, y = uResponseRange, z = uIdleWave, w = uAlphaK
  float4 P2;   // x = uSubBass,  y = uBass,    z = uLowMid,  w = uMid
  float4 P3;   // x = uHighMid,  y = uEnergy,  z = uPresence, w = uBrilliance
  float4 P4;   // x = uAir,      y = uBrightness, z = uGlowIntensity, w = uPeakEnabled
  float4 P5;   // x = uPeakIntensity, y = uTime
  float4 uCoolCore;
  float4 uWarmCore;
  float4 uPeakColor;
};

struct VSOut {
  float4 pos : SV_POSITION;
  float2 uv  : TEXCOORD0;
};

// 解包成与原作品同名的量，下面的正文与 fragment.glsl 保持逐行对应
#define uRes            (P0.xy)
#define uStyleMode      (P0.z)
#define uBeamInset      (P0.w)
#define uAudioIntensity (P1.x)
#define uResponseRange  (P1.y)
#define uIdleWave       (P1.z)
#define uAlphaK         (P1.w)
#define uSubBass        (P2.x)
#define uBass           (P2.y)
#define uLowMid         (P2.z)
#define uMid            (P2.w)
#define uHighMid        (P3.x)
#define uEnergy         (P3.y)
#define uPresence       (P3.z)
#define uBrilliance     (P3.w)
#define uAir            (P4.x)
#define uBrightness     (P4.y)
#define uGlowIntensity  (P4.z)
#define uPeakEnabled    (P4.w)
#define uPeakIntensity  (P5.x)
#define uTime           (P5.y)

// 主题色在 Three 的 Color 里是**线性**值，而这一份是自定义 ShaderMaterial、
// 直接把颜色写进帧缓冲（不参与 Three 的输出色彩空间转换）。
// 所以必须先转成 sRGB，否则整块画面会比页面上那块背景色暗一大截。
float3 lin2srgb(float3 c) {
  c = clamp(c, 0.0, 1.0);
  return lerp(c * 12.92,
              1.055 * pow(c, 1.0 / 2.4) - 0.055,
              step(0.0031308, c));
}

float4 PSMain(VSOut i) : SV_Target {
  float2 uv = i.uv;

  float3 cCool = lin2srgb(uCoolCore.rgb);
  float3 cWarm = lin2srgb(uWarmCore.rgb);
  float3 cPk   = lin2srgb(uPeakColor.rgb);

  float asp = uRes.x / max(uRes.y, 1.0);

  // ---------------------------------------------------------------
  //  1. 驱动量：作品自己的低频齐弹
  // ---------------------------------------------------------------
  float low = uSubBass * 1.20 + uBass * 0.85 + uEnergy * 0.25;

  // ⚠ drive 必须在这里算，**不能写进任何一个分支里**。
  //   它是两种风格共享的那一根驱动量：风格① 拿它当柱高，风格② 拿它当闪烁强度。
  float drive = clamp(low * uAudioIntensity * uResponseRange, 0.0, 1.70);

  float h     = 0.0;
  float dn    = 0.0;
  float head2 = 0.0;
  float amp   = 0.0;
  float flash = 0.0;
  float3 c    = cCool;

  if (uStyleMode < 0.5) {
    // ================= 风格①：光柱上下跳动 =================
    h     = drive * 0.62;
    dn    = clamp(drive / 1.30, 0.0, 1.0);
    head2 = smoothstep(0.50, 0.95, dn);
    amp   = (0.50 + drive * 0.85) * uGlowIntensity;
    amp  *= smoothstep(0.0, 0.16, drive + uIdleWave);
    c     = lerp(cCool, cWarm, clamp(drive * 0.55, 0.0, 1.0));
  } else {
    // ============ 风格②：常驻光柱 · 节拍闪色 ============
    h     = 1.0;
    head2 = 0.0;

    flash = smoothstep(0.18, 0.95, drive);

    float3 cFlash = lerp(cWarm, cPk, clamp(uPeakEnabled * uPeakIntensity, 0.0, 1.0));
    c = lerp(cCool, cFlash, flash);

    amp = 0.85 * uGlowIntensity * (1.0 + flash * 0.85) * lerp(1.0, 0.50, uIdleWave);
  }

  // 空闲呼吸：安静时光柱轻轻起伏。
  // ⚠ 必须是「有基数的叠加」，不能写成两个正弦的乘积：乘积的谷底会归零，
  //   光柱会周期性整个消失。
  float breathe = 0.66 + 0.20 * sin(uTime * 0.55) + 0.14 * sin(uTime * 0.23 + 1.7);
  h += max(0.0, breathe) * uIdleWave * 0.48;

  // 强调色（只作用于风格①）：只在跳得高时给柱身基色染色。
  // ⚠ 必须染 c 而不是染最终颜色 —— 染最终颜色会把没点亮的地方一起泛色。
  float pk = clamp(dn * dn * uPeakEnabled * uPeakIntensity, 0.0, 1.0);
  c = lerp(c, cPk, pk * 0.45);

  // ---------------------------------------------------------------
  //  2. 造型：两根屏幕边光柱
  // ---------------------------------------------------------------
  float WX = 0.26;                        // 横向射程（画面高为单位）
  float EY = lerp(0.012, 0.14, head2);    // 顶端纵向柔化半宽
  float3 col = float3(0.0, 0.0, 0.0);

  [unroll]
  for (int i = 0; i < 2; i++) {
    float e0 = ((i == 0) ? uv.x : (1.0 - uv.x)) - uBeamInset;
    float dx = e0 * asp;

    float dy = max(0.0, uv.y - h);
    float d  = length(float2(dx, dy));

    // ⚠ 用 smoothstep 而不是 pow：smoothstep 在两端导数都为 0，
    //   靠近边缘时「平缓地贴到 0」，射程才会稳定钉在 WX 上。
    float g  = 1.0 - smoothstep(0.0, WX, d);
    float ay = 1.0 - smoothstep(0.0, EY, uv.y - h);

    float halo = 0.0;
    if (uStyleMode < 0.5) {
      float rx = dx / WX;
      float ry = (uv.y - h) / (WX * 1.25);
      float rr = length(float2(rx, ry));
      // ⚠ 这里用 pow 而不是 smoothstep：smoothstep 在 rr 小时导数为 0，
      //   会形成一个亮度平坦的圆盘 + 一条清晰的圆边（像贴了个圆上去）。
      halo = pow(max(1.0 - rr, 0.0), 2.2) * head2;
    }

    float grad = lerp(0.55, 1.0, clamp(uv.y / max(h, 0.001), 0.0, 1.0));

    col += c * (g * ay * grad + halo * 0.62) * amp;
  }

  // ---------------------------------------------------------------
  //  3. 软饱和 + 输出
  // ---------------------------------------------------------------
  col = col / (1.0 + col * 0.5);

  // ── 桌面版与网页版的唯一区别就在这里 ──────────────────────────────
  // 网页版：gl_FragColor = vec4(bg + col, 1.0);   （不透明，带主题底色）
  // 桌面版：把 col 当成**预乘 alpha 的光**输出给 DWM 合成器。
  //
  //   RGB  = col              光本身的颜色（满亮度，不被 alpha 缩）
  //   A    = 光强 * uAlphaK   只用来决定「压暗桌面多少」
  //
  // 交换链显式声明为 DXGI_ALPHA_MODE_PREMULTIPLIED，DirectComposition 按
  // 预乘规则合成：结果 = RGB + 桌面 * (1 - A)。
  //   uAlphaK = 0 → 结果 = col + 桌面      ← 纯加光，像真光打上去
  //   uAlphaK = 1 → 结果 = col + 桌面*(1-A) ← 柱心实心，会遮挡桌面
  //
  // ⚠ 末尾的 +0.004（1/255）是保险：uAlphaK=0 且 col 全 0 时整层 alpha 会变成 0，
  //   部分合成路径会把「完全透明的图层」整层丢掉，光柱就凭空消失了。
  //   代价是桌面被压暗 0.4%，肉眼不可见。
  float a = max(max(col.r, col.g), col.b) * uAlphaK + 0.004;
  return float4(col, a);
}
)HLSL";
