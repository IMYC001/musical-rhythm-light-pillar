package com.lightpillar

/**
 * GLES 着色器。
 *
 * ★ 片元着色器是原作品 `_src/fragment.glsl` 的**原文** —— 不是翻译件。
 *   原作品本来就是 WebGL（GLSL ES），安卓的 GLES 直接吃。
 *   Windows 版是把它翻成 HLSL 的（因为那台机器没有硬件 OpenGL 驱动），
 *   所以**安卓版的保真度实际上比 Windows 版更高**：少了一道翻译。
 *
 * 只做了三处必要改动，其余逐字保留（包括所有 ⚠ 注释里标出来的坑）：
 *
 *   1. 加 `precision highp float;`（GLES 必须显式声明，WebGL 有默认值）；
 *   2. 去掉 `uBaseColor1` / `bg` —— 悬浮窗是**纯加光**，不画主题底色。
 *      原作品是 `gl_FragColor = vec4(bg + col, 1.0)`（不透明、带底色），
 *      这里输出预乘色 `(col, a)`，把 col 当成**光**交给合成器；
 *   3. 末尾输出改成预乘 alpha（见下）。
 *
 * ── 关于「纯加光」在安卓上到底成不成立 ──────────────────────────────
 * Windows 版：交换链声明 DXGI_ALPHA_MODE_PREMULTIPLIED，
 *   DirectComposition 按预乘规则合成：`结果 = RGB + 桌面 * (1 - A)`
 * 安卓版：悬浮窗 Surface 设为 TRANSLUCENT，
 *   SurfaceFlinger 按 src-over 合成：`out = src.rgb + dst.rgb * (1 - src.a)`
 *
 * **两个公式完全一样。** 所以 uAlphaK 这个参数在安卓上有完全相同的语义：
 *   uAlphaK = 0 → 结果 ≈ col + 桌面      ← 纯加光，像真光打上去
 *   uAlphaK = 1 → 结果 = col + 桌面*(1-A) ← 柱心实心，会遮挡桌面
 *
 * ⚠ 末尾的 +0.004（1/255）是保险：uAlphaK=0 且 col 全 0 时整层 alpha 会变成 0，
 *   部分合成路径会把「完全透明的图层」整层丢掉，光柱就凭空消失了。
 *   代价是桌面被压暗 0.4%，肉眼不可见。
 *
 * ⚠ 一个尚未在真机验证的风险：预乘色的定义要求 RGB ≤ A，而这里输出的是
 *   RGB = col（满亮）、A 却可能远小于 max(col)（uAlphaK < 1 时）。
 *   DirectComposition 容忍这种输入（它就是按公式算的）；SurfaceFlinger
 *   在部分设备的硬件合成器（HWC）上可能不按公式走。
 *   真机上若出现「光柱颜色发暗/被削平」，把「加光强度」滑杆（= uAlphaK）
 *   往 1 推即可退回守规矩的路径。见交付说明里的「已知风险」。
 */
object Shaders {

    /** 顶点：一块全屏四边形，aPos 已经是 NDC（-1..1）。 */
    const val VERT = """
attribute vec2 aPos;
varying vec2 vUv;

void main() {
  // 原作品用 Three 的 BoxGeometry +Z 面、uv 是干净的 0..1，
  // 所以 (uv - 0.5) * 2 正好是 NDC。这里反过来：uv = aPos * 0.5 + 0.5。
  // ⚠ 方向必须和原作品一致：GL 的 NDC 是 y 向上，uv.y = 0 在**下边**。
  //   Windows 版（D3D11）也是这个方向 —— 反过来光柱会上下颠倒，
  //   「柱底偏暗、柱顶最亮」会变成「柱顶偏暗」。
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}
"""

    /**
     * 片元：原作品 fragment.glsl 原文（改动见文件头注释）。
     */
    const val FRAG = """
precision highp float;

uniform vec2  uRes;          // 画布尺寸（用于纵横比换算）
uniform float uTime;
uniform float uStyleMode;    // 0 = 光柱上下跳动，1 = 常驻光柱 · 节拍闪色
uniform float uBeamInset;    // 光柱轴心离屏幕两边的距离（屏幕宽为单位）

uniform float uAudioIntensity;
uniform float uResponseRange;
uniform float uIdleWave;

uniform float uSubBass;
uniform float uBass;
uniform float uLowMid;
uniform float uMid;
uniform float uHighMid;

uniform float uEnergy;
uniform float uPresence;
uniform float uBrilliance;
uniform float uAir;
uniform float uBrightness;

uniform vec3  uCoolCore;
uniform vec3  uWarmCore;
uniform vec3  uPeakColor;
uniform float uGlowIntensity;
uniform float uPeakEnabled;
uniform float uPeakIntensity;
uniform float uAlphaK;       // 安卓新增：只用来决定「压暗桌面多少」
uniform float uDebugMode;    // ★ 诊断开关（见文件末尾）

varying vec2 vUv;

// 主题色在 Three 的 Color 里是**线性**值，而这一份是自定义 ShaderMaterial、
// 直接把 gl_FragColor 写进帧缓冲（不参与 Three 的输出色彩空间转换）。
// 所以必须先转成 sRGB，否则整块画面会比页面上那块背景色暗一大截。
vec3 lin2srgb(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  return mix(c * 12.92,
             1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
             step(vec3(0.0031308), c));
}

void main() {
  /* ★★★ 诊断开关（uDebugMode，存 ini 的 [android] section，用 adb 直接改就能测）。
     存在的理由：真机上「光柱不显示」有三个完全不同的成因，光看截图分不出来 ——
       ① 着色器算出来就是黑的       → 读数全 0
       ② 算对了但被合成器丢了       → 读数非零、屏幕全黑
       ③ 图层压根没上屏             → 读数非零、屏幕全黑（和 ② 在截图上**一模一样**）
     glReadPixels 只能分开 ① 和 ②③；要再分开 ② 和 ③，只能靠**让着色器输出不透明**：
     不透明输出若能显示 ⇒ 图层在屏上，是 alpha/预乘的问题（②）；
     仍不显示 ⇒ 图层根本没被合成（③），再怎么调 alpha 都没用。
     ⚠ 这些分支必须放在最前面，且不能依赖任何音频量 —— 诊断要能在静音下工作。 */
  if ((uDebugMode > 0.5 && uDebugMode < 6.5) || (uDebugMode > 9.5 && uDebugMode < 11.5)) {
    /* 每个模式都是一次**可判读的实验**，看屏幕就能读出合成器的公式：
         1/2/3 = 纯红/绿/白，A=1  → 测「图层在不在屏上」
         4 = 白 A=0.5             → 预乘得 255(饱和白)；直乘得 ~129(中灰)
         5 = 白 A=0.25            → 预乘得 255；直乘得 ~65(暗灰)
         6 = 灰0.5 A=0.5          → 合法的预乘输入(RGB=A)，两种公式都该得 ~128
       4 和 6 是关键：4 若白、6 若灰 ⇒ 预乘；4 若中灰 ⇒ 直乘(非预乘)。 */
    vec4 dbg = vec4(1.0, 0.0, 0.0, 1.0);
    if (uDebugMode > 1.5 && uDebugMode < 2.5) dbg = vec4(0.0, 1.0, 0.0, 1.0);
    else if (uDebugMode > 2.5 && uDebugMode < 3.5) dbg = vec4(1.0, 1.0, 1.0, 1.0);
    else if (uDebugMode > 3.5 && uDebugMode < 4.5) dbg = vec4(1.0, 1.0, 1.0, 0.5);
    else if (uDebugMode > 4.5 && uDebugMode < 5.5) dbg = vec4(1.0, 1.0, 1.0, 0.25);
    else if (uDebugMode > 5.5 && uDebugMode < 6.5) dbg = vec4(0.5, 0.5, 0.5, 0.5);
    /* ★ 10/11 是**恒定输出**的判别实验，专门用来测「RGB 远大于 A」这种输入。
       之前用真实光柱做对比被测试音的音量波动污染了（drive 在 0.004~0.26 之间跳，
       两次截图时刻不同就得出不同结论）—— 判别实验必须用不随时间变的输入。
         10 = RGB(0.30,0.15,0.45) A=0.034 → 预乘预测 R≈79；直乘预测 R≈10
         11 = 同上但 A=0.50             → 预乘预测 R≈78；直乘预测 R≈40
       10 与 11 若都≈78 ⇒ 预乘；若 10≈10、11≈40 ⇒ 直乘（非预乘）。 */
    else if (uDebugMode > 9.5 && uDebugMode < 10.5) dbg = vec4(0.30, 0.15, 0.45, 0.034);
    else if (uDebugMode > 10.5 && uDebugMode < 11.5) dbg = vec4(0.30, 0.15, 0.45, 0.50);
    gl_FragColor = dbg;
    return;
  }

  vec2 uv = vUv;

  vec3  cCool = lin2srgb(uCoolCore);
  vec3  cWarm = lin2srgb(uWarmCore);
  vec3  cPk   = lin2srgb(uPeakColor);

  float asp = uRes.x / max(uRes.y, 1.0);

  // ---------------------------------------------------------------
  //  1. 驱动量：作品自己的低频齐弹
  //     超低音给主体、低音给冲击、整体能量兜一层底。
  // ---------------------------------------------------------------
  float low = uSubBass * 1.20 + uBass * 0.85 + uEnergy * 0.25;

  // ⚠ drive 必须在这里算，**不能写进任何一个分支里**。
  //   它是两种风格**共享**的那一根驱动量：风格① 拿它当柱高，风格② 拿它当闪烁强度
  //   —— 「闪的强度就好像上下跳动的强度一样」这句话就是靠这一点成立的。
  //   踩过一次：写在风格① 的分支里，风格② 里 drive 一直是初始值 0，
  //   于是 flash 恒为 0、颜色纹丝不动（uniform 全是对的，画面就是不变，很难查）。
  float drive = clamp(low * uAudioIntensity * uResponseRange, 0.0, 1.70);

  /* ★ 诊断模式 12：把驱动量钉死成一个常数。
     为什么要这一手：用真实光柱做「屏幕 vs framebuffer」对照时，测试音的音量在
     0.004~0.26 之间跳，两次采样时刻不同就得出互相矛盾的结论（本项目真机上
     就吃过这个亏：一度以为「合成器把光吃了」，其实是采样时机错开）。
     固定驱动量之后，渲染结果不随时间变，屏幕与 framebuffer 才能逐像素对照。 */
  if (uDebugMode > 11.5 && uDebugMode < 12.5) drive = 0.90;
  /* 12/13/14 = 固定 0.90 / 0.20 / 0.10。
     ⚠ 0.20 这个数是**有出处的**，不是我拍的：原生版 `selftest-鼓点/自检报告.json`
       用鼓点测试音跑出的峰值是 subBass=0.093 / bass=0.066 / energy=0.077，
       代入 drive 公式 = 0.093*1.2 + 0.066*0.85 + 0.077*0.25 ≈ 0.19。
       也就是说「测试音下的真实水平」就是 0.2 上下 —— 拿它当判据才有意义。 */
  else if (uDebugMode > 12.5 && uDebugMode < 13.5) drive = 0.20;
  else if (uDebugMode > 13.5 && uDebugMode < 14.5) drive = 0.10;

  float h     = 0.0;
  float dn    = 0.0;
  float head2 = 0.0;
  float amp   = 0.0;
  float flash = 0.0;
  vec3  c     = cCool;

  if (uStyleMode < 0.5) {
    // ================= 风格①：光柱上下跳动 =================
    h     = drive * 0.62;
    // 占满程比例：只用来决定顶端形状的圆化程度
    dn    = clamp(drive / 1.30, 0.0, 1.0);
    // 低幅时 head2 严格为 0 → 顶端是一条干净的平边；
    // 只有接近满幅时顶端才圆起来。
    head2 = smoothstep(0.50, 0.95, dn);
    amp   = (0.50 + drive * 0.85) * uGlowIntensity;
    // 安静时不留半亮的短柱：从 0 淡入，时间上没有硬边。
    // 加 uIdleWave 是为了空闲态仍然过得去（见下面那条）。
    amp  *= smoothstep(0.0, 0.16, drive + uIdleWave);
    // 渐变轴就是驱动量：安静偏冷色，越响越靠暖色
    c     = mix(cCool, cWarm, clamp(drive * 0.55, 0.0, 1.0));
  } else {
    // ============ 风格②：常驻光柱 · 节拍闪色 ============
    // 柱高恒为满高、顶端不做圆化 —— 律动完全落在颜色上。
    h     = 1.0;
    head2 = 0.0;

    // ⚠ 用的是和风格① **同一根** drive（不是另起一套阈值），
    //   所以「闪的强度」天然就等于「跳的强度」—— 同一段音乐下两者同起同落。
    //   实测这段合成音乐：安静基线 drive ≈ 0.12、鼓点峰值 ≈ 1.15（10 倍摆幅），
    //   用 0.18~0.95 拉一道对比 → 基线压到 0、鼓点冲满、之后约 0.5 秒回落。
    //   一记节拍闪一下，而不是缓慢地来回渐变。
    flash = smoothstep(0.18, 0.95, drive);

    // 闪到的颜色用**作品自己的强调色**（默认「霁紫」主题是橙）。
    // ⚠ 不能闪到主题暖色 uWarmCore —— 15 套主题实测：
    //   「柱色 ↔ 暖色」的色距太小（默认主题 (160,89,243) vs (211,137,255)，
    //   两个都是紫），哪怕 0→1 推满也看不出换色。
    //   强调色的色距对 14/15 套主题是 100~316（明确的换色）。
    //   关掉强调色就退回主题暖色，只是变含蓄，不会变成没有律动。
    vec3 cFlash = mix(cWarm, cPk, clamp(uPeakEnabled * uPeakIntensity, 0.0, 1.0));
    c = mix(cCool, cFlash, flash);

    // 除了换色，亮度也要跟着闪一下 —— 只有换色那是「变色」，不是「闪」。
    // 底亮度里不含驱动量，所以安静时是稳稳的一根常驻柱子。
    amp = 0.85 * uGlowIntensity * (1.0 + flash * 0.85) * mix(1.0, 0.50, uIdleWave);
  }

  // 空闲呼吸：安静时光柱轻轻起伏（作品自己的空闲波浪设置）。
  // ⚠ 必须是「有基数的叠加」，不能写成两个正弦的**乘积**：
  //   乘积的谷底会归零，光柱会周期性整个消失（冒烟测试直接抓到过 ——
  //   空闲态截图正好落在谷底，量出来和背景一模一样）。
  //   两个不成比例（0.55 / 0.23）的频率叠起来，免得看出匀速的机械感。
  float breathe = 0.66 + 0.20 * sin(uTime * 0.55) + 0.14 * sin(uTime * 0.23 + 1.7);
  h += max(0.0, breathe) * uIdleWave * 0.48;

  // 强调色（只作用于风格①）：只在跳得高时给**柱身基色**染色。
  // 风格② 的闪色已经在上面直接把 c 推到强调色了，这里 dn 恒为 0、不重复作用。
  // ⚠ 必须染 c 而不是染最终颜色 —— 染最终颜色会把没点亮的地方一起泛色，
  //   整片画面蒙上一层暖黄（上一版踩过这个坑）。
  float pk = clamp(dn * dn * uPeakEnabled * uPeakIntensity, 0.0, 1.0);
  c = mix(c, cPk, pk * 0.45);

  // ---------------------------------------------------------------
  //  2. 造型：两根屏幕边光柱
  // ---------------------------------------------------------------
  /* 横向射程（画面**高**为单位）。
     ★★ 必须随纵横比收缩，不能写死 0.26 —— 这是「光柱离边距离拉到最低光柱不消失」
        的真正原因，不是滑块范围的问题，也不是回归。

     推导（三行就够）：
       · 轴心在 uv.x = uBeamInset，横向偏移 e0 = |uv.x - inset|，再乘 asp 换到高度尺度。
       · 光柱能照到的最远点离轴心 WX，即 uv.x 方向上的射程是 WX / asp。
       · 于是「光柱整根退出屏幕左边」的条件是
             |inset| * asp >= WX      ⟺      |inset| >= WX / asp
       · 原作品写死 WX = 0.26，阈值 = 0.26 / asp：
             16:9  (asp = 1.778) → 阈值 0.1462  < 0.15  ⇒ 拉到 -0.15 正好离场 ✓
             竖屏  (asp = 0.75)  → 阈值 0.3467  > 0.15  ⇒ 滑块走到底也还剩一段 ✗

     修法：把 WX 按 asp 等比缩，让 **WX / asp 恒等于 16:9 的 0.1462**。
       宽屏（asp >= 1.778）原样不动；窄屏按比例收窄。
       一举两得 —— 除了让阈值在所有纵横比下一致，还顺带把「光柱占屏幕宽度的比例」
       也钉住了（原作品在竖屏下两根柱子会各占屏幕宽度的 1/3，糊在一起）。
       这条正是「所有分辨率通用」要求的：几何随屏幕形状等比，而不是写死像素/比例。

     ⚠ 别改成「只把滑块下限从 -0.15 放大」：那样 16:9 上光柱会提前离场，
       桌面版/Windows 版手感就对不上了（阈值必须在每个纵横比下都是 0.1462）。 */
  float WX = 0.26 * min(1.0, asp / 1.778);
  float EY = mix(0.012, 0.14, head2);     // 顶端纵向柔化半宽
  vec3  col = vec3(0.0);

  for (int i = 0; i < 2; i++) {
    // 轴心在屏幕左边 (x=0) / 右边 (x=1)；uBeamInset 把轴心整体往画面里挪。
    // 不取绝对值、也不加内侧遮罩：光柱不是一堵墙，轴心两侧都该有光。
    float e0 = ((i == 0) ? uv.x : (1.0 - uv.x)) - uBeamInset;
    // 换算成和竖直同一个尺度：于是射程以**画面高度**为单位，
    // 窗口拉宽时光柱的像素宽度不变，不会摊开。
    float dx = e0 * asp;

    // 到「从轴心往上、长 h 的竖直段」的距离：
    //   uv.y < h 时是纯横向；uv.y > h 时是以光柱顶端为心的圆。
    float dy = max(0.0, uv.y - h);
    float d  = length(vec2(dx, dy));

    // 从轴心连续衰减，没有等宽全亮核心。
    // ⚠ 用 smoothstep 而不是 pow：smoothstep 在两端导数都为 0，
    //   靠近边缘时「平缓地贴到 0」，射程才会稳定钉在 WX 上。
    float g  = 1.0 - smoothstep(0.0, WX, d);
    // 顶端截断：低幅时 EY 极小 → 干净的平边；满幅时 EY 大 → 向上发散
    float ay = 1.0 - smoothstep(0.0, EY, uv.y - h);

    // 顶端那团自然发散的圆光（只有跳动的风格才有）
    float halo = 0.0;
    if (uStyleMode < 0.5) {
      // 纵向半径取横向的 1.25 倍（椭圆）→ 光向上发散；
      // 横向半径严格等于 WX（和柱身同一个数）→ 射程不变。
      float rx = dx / WX;
      float ry = (uv.y - h) / (WX * 1.25);
      float rr = length(vec2(rx, ry));
      // ⚠ 这里用 pow 而不是 smoothstep：smoothstep 在 rr 小时导数为 0，
      //   会形成一个亮度平坦的圆盘 + 一条清晰的圆边（像贴了个圆上去）。
      halo = pow(max(1.0 - rr, 0.0), 2.2) * head2;
    }

    // 越靠近顶端越亮
    float grad = mix(0.55, 1.0, clamp(uv.y / max(h, 0.001), 0.0, 1.0));

    col += c * (g * ay * grad + halo * 0.62) * amp;
  }

  // ---------------------------------------------------------------
  //  3. 软饱和 + 输出
  // ---------------------------------------------------------------
  col = col / (1.0 + col * 0.5);

  // ── 原作品与悬浮窗的唯一区别就在这里 ────────────────────────────
  // 原作品：gl_FragColor = vec4(bg + col, 1.0);   （不透明，带主题底色）
  // 悬浮窗：把 col 当成**预乘 alpha 的光**输出给合成器。
  //
  //   RGB  = col              光本身的颜色（满亮度，不被 alpha 缩）
  //   A    = 光强 * uAlphaK   只用来决定「压暗桌面多少」
  //
  // SurfaceFlinger 按 src-over 合成：结果 = RGB + 桌面 * (1 - A)。
  // 于是 uAlphaK = 0 就是「纯加光」，和 Windows 版逐字同义。
  float a = max(max(col.r, col.g), col.b) * uAlphaK + 0.004;

  /* ★ 诊断模式 7/8：验证「光柱画对了，但小 alpha 被合成器丢掉」这个假设。
       7 = 同样几何、alpha 直接给 1   → 应看到实心光柱（证明几何与上屏都正常）
       8 = 同样几何、alpha 抬到 ≥0.5  → 若 7/8 都可见而 0 不可见，
           说明问题就在「alpha 太小」这一条上，与几何/图层无关。 */
  if (uDebugMode > 6.5 && uDebugMode < 7.5) { gl_FragColor = vec4(col, 1.0); return; }
  if (uDebugMode > 7.5 && uDebugMode < 8.5) { gl_FragColor = vec4(col, max(a, 0.5)); return; }

  gl_FragColor = vec4(col, a);
}
"""
}
