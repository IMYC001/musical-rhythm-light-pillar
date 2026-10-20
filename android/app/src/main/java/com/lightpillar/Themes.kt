package com.lightpillar

/**
 * 一套主题。数值是从作品 bundle 自动抽取的 Three Color **线性**值，
 * 着色器里会过一遍 lin2srgb。
 *
 * ⚠ 和 Windows 版 `src/data_themes.h` 逐字一致，不要手改。
 *   两边的渲染结果要能逐像素对照，色值差一位都会变成假差异。
 */
class LpTheme(
    val key: String,
    val name: String,
    /** uBaseColor1 —— 只用于「主题底色」。悬浮窗是纯加光，不画它。 */
    val base: FloatArray,
    /** uCoolCore —— 柱身冷色（安静时的颜色） */
    val cool: FloatArray,
    /** uWarmCore —— 柱身暖色（越响越靠它） */
    val warm: FloatArray,
    /** uPeakColor —— 强调色 / 节拍闪色 */
    val peak: FloatArray,
    /** uGlowIntensity —— 光晕强度 */
    val glow: Float,
)

val LP_THEMES: List<LpTheme> = listOf(
    LpTheme("nocturnal", "霁紫",
        floatArrayOf(0.005f, 0.008f, 0.025f),
        floatArrayOf(0.350f, 0.100f, 0.900f),
        floatArrayOf(0.650f, 0.250f, 1.000f),
        floatArrayOf(1.000f, 0.550f, 0.050f), 1.0000f),

    LpTheme("ocean-deep", "沧蓝",
        floatArrayOf(0.002f, 0.008f, 0.028f),
        floatArrayOf(0.000f, 0.250f, 1.000f),
        floatArrayOf(0.150f, 0.550f, 1.000f),
        floatArrayOf(1.000f, 0.750f, 0.100f), 1.1000f),

    LpTheme("arctic-aurora", "冰蓝",
        floatArrayOf(0.003f, 0.015f, 0.022f),
        floatArrayOf(0.000f, 0.750f, 0.850f),
        floatArrayOf(0.200f, 1.000f, 0.850f),
        floatArrayOf(1.000f, 0.250f, 0.350f), 1.2500f),

    LpTheme("cyber-forest", "碧翠",
        floatArrayOf(0.003f, 0.018f, 0.005f),
        floatArrayOf(0.000f, 0.850f, 0.350f),
        floatArrayOf(0.400f, 1.000f, 0.300f),
        floatArrayOf(1.000f, 0.200f, 0.500f), 1.3000f),

    LpTheme("golden-hour", "流金",
        floatArrayOf(0.018f, 0.015f, 0.005f),
        floatArrayOf(0.850f, 0.600f, 0.050f),
        floatArrayOf(1.000f, 0.920f, 0.350f),
        floatArrayOf(0.200f, 0.500f, 1.000f), 1.2000f),

    LpTheme("ember-fire", "余烬",
        floatArrayOf(0.022f, 0.008f, 0.002f),
        floatArrayOf(1.000f, 0.450f, 0.000f),
        floatArrayOf(1.000f, 0.780f, 0.150f),
        floatArrayOf(0.100f, 0.400f, 1.000f), 1.5000f),

    LpTheme("crimson-sunset", "赤焰",
        floatArrayOf(0.025f, 0.003f, 0.005f),
        floatArrayOf(1.000f, 0.050f, 0.080f),
        floatArrayOf(1.000f, 0.350f, 0.200f),
        floatArrayOf(0.100f, 0.900f, 0.700f), 1.4000f),

    LpTheme("coral-mirage", "霞粉",
        floatArrayOf(0.020f, 0.006f, 0.010f),
        floatArrayOf(1.000f, 0.250f, 0.300f),
        floatArrayOf(1.000f, 0.550f, 0.550f),
        floatArrayOf(0.100f, 0.700f, 1.000f), 1.3000f),

    LpTheme("neon-tokyo", "幻紫",
        floatArrayOf(0.010f, 0.002f, 0.025f),
        floatArrayOf(1.000f, 0.050f, 0.600f),
        floatArrayOf(1.000f, 0.250f, 0.850f),
        floatArrayOf(0.950f, 1.000f, 0.150f), 1.6000f),

    LpTheme("minimal-monochrome", "水墨",
        floatArrayOf(0.012f, 0.012f, 0.012f),
        floatArrayOf(0.800f, 0.800f, 0.800f),
        floatArrayOf(1.000f, 1.000f, 1.000f),
        floatArrayOf(1.000f, 1.000f, 1.000f), 0.7000f),

    LpTheme("teal-depth", "幽青",
        floatArrayOf(0.002f, 0.018f, 0.020f),
        floatArrayOf(0.000f, 0.550f, 0.550f),
        floatArrayOf(0.200f, 0.850f, 0.750f),
        floatArrayOf(1.000f, 0.450f, 0.150f), 1.2000f),

    LpTheme("lavender-dream", "薰衣草",
        floatArrayOf(0.012f, 0.008f, 0.022f),
        floatArrayOf(0.550f, 0.350f, 0.850f),
        floatArrayOf(0.750f, 0.550f, 1.000f),
        floatArrayOf(1.000f, 0.800f, 0.250f), 1.1000f),

    LpTheme("cherry-blossom", "樱",
        floatArrayOf(0.018f, 0.005f, 0.012f),
        floatArrayOf(1.000f, 0.550f, 0.650f),
        floatArrayOf(1.000f, 0.720f, 0.780f),
        floatArrayOf(0.250f, 0.900f, 0.550f), 1.1500f),

    LpTheme("copper-forge", "锻铜",
        floatArrayOf(0.020f, 0.010f, 0.005f),
        floatArrayOf(0.850f, 0.450f, 0.200f),
        floatArrayOf(1.000f, 0.650f, 0.300f),
        floatArrayOf(0.300f, 0.650f, 0.350f), 1.3000f),

    LpTheme("mint-fresh", "薄荷",
        floatArrayOf(0.003f, 0.020f, 0.015f),
        floatArrayOf(0.300f, 0.900f, 0.650f),
        floatArrayOf(0.500f, 1.000f, 0.800f),
        floatArrayOf(1.000f, 0.300f, 0.550f), 1.2000f),
)

fun themeAt(index: Int): LpTheme = LP_THEMES[index.coerceIn(0, LP_THEMES.size - 1)]
