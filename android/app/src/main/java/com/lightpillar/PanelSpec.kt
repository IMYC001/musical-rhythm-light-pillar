package com.lightpillar

/**
 * 设置页的一项。范围、步长、小数位一律照抄 Windows 版 `main.cpp` 的
 * `addSlider / addSwitch / addSelect / addSegment` 调用 ——
 * 两边面板的手感要一致，数值范围差一点用户就会觉得「怎么推到底了还没反应」。
 */
sealed class PItem {
    class Sec(val title: String) : PItem()

    class Slider(
        val label: String,
        val key: String,
        val lo: Float,
        val hi: Float,
        val step: Float,
        val dec: Int,
        val unit: String = "",
        /** 显示值 = 实际值 × mul（对应 Windows 版的 dispMul，例如 0.85 → 85%） */
        val mul: Float = 1f,
    ) : PItem()

    class Switch(val label: String, val key: String) : PItem()

    /**
     * 下拉。
     *
     * ⚠ 两种语义必须区分开：`styleMode` / `mapMode` / `bandMode`
     *   存的就是**下标**；而 `fpsLimit` / `anaHz` 存的是**实际值**（30/60/250）。
     *   混了的话表现是「选了 120fps，存进去 2」或者「下拉显示错项」。
     *
     * ⚠⚠ `themeIndex` 是**第三种**，也是最容易错的一种：它的选项列表多了一项
     *   占位（第 0 项 =「自动轮询」），而且第 0 项不是「值 = 0」，
     *   它要**同时**改 `themeAuto` 这个布尔键。
     *   Windows 版的做法（`main.cpp`）：
     *     ```
     *     g_themeSel = themeAuto ? 0 : (themeIndex + 1);          // 显示
     *     if (sel <= 0) themeAuto = true;                         // 选择
     *     else { themeAuto = false; themeIndex = sel - 1; themeClock = 0; }
     *     ```
     *   这边如果按通用下拉直接存下标（`themeIndex = which`），就会**错位一格**：
     *   选「霁紫」渲染出来是「沧蓝」。处理在 [PanelBuilder.selectView] 里特判。
     */
    class Select(
        val label: String,
        val key: String,
        val opts: List<String>,
        /** 选项对应的实际值；为 null 表示「存的就是下标」。 */
        val vals: List<Double>? = null,
    ) : PItem()

    class Hint(val lines: List<String>) : PItem()

    /**
     * 一个纯动作按钮 —— 不是参数，不落任何键值。
     *
     * 为什么需要这一种：参考实现参数页的标题行里有一个「恢复默认参数」按钮
     * （`桥接层` 1100~1116 行），Windows 版对应 `ID_PANEL_RESET`。
     * 它既不是滑块也不是开关，塞进任何一类参数控件里都会变成「一个存不进值的死控件」。
     *
     * ⚠ 位置有讲究：它必须落在 [PANEL_HEAD] 的末尾、**预设区之前** ——
     *   照参考实现的顺序（参数页最上面是「恢复默认参数」，紧接着预设区，
     *   参考实现注释原话：「它管的是整套参数，比下面任何一条具体参数都更靠前」）。
     *   所以 [PANEL] 被切成了两段，中间那段留给 [PresetPanel]，见 MainActivity。
     */
    class Button(val label: String, val id: String) : PItem()
}

/** 按钮动作的 id。 */
object BtnId {
    const val RESET = "reset"
}

/** 主题下拉的选项：第 0 项是「自动轮询」占位，1..N 对应 LP_THEMES[0..N-1]。 */
fun themeOptions(): List<String> = buildList {
    add("自动轮询")
    for (t in LP_THEMES) add(t.name)
}

/**
 * 面板前半段：音频响应校准 + 「恢复默认设置」按钮。
 *
 * ★ 为什么把清单切成两段：预设区（[PresetPanel]）必须插在「恢复默认设置」**后面**、
 *   「外观」**前面**，而预设区是另一个类在 MainActivity 里单独 build 的。
 *   合成一份清单就没法在中间插东西了。
 *
 * ⚠ 「音频来源」这一整段（标题 + 说明 + 下拉）**不在**这里 —— 它是安卓特有的，
 *   而且那个下拉不在 27 键清单里（硬塞进来的话 `byName[key]` 查不到，控件会是死的）。
 *   由 MainActivity 用 [PanelBuilder.sectionHeader] / [PanelBuilder.hintBlock]
 *   单独装配。这里曾经把标题留在清单里、控件在外面加 ——
 *   结果「音频来源」这个标题画在了「音频输入」那一行的**下面**。
 */
val PANEL_HEAD: List<PItem> = buildList {

    /* ==================== 音频响应校准 ==================== */
    add(PItem.Sec("音频响应校准"))
    add(PItem.Slider("增益", "gainDb", -12f, 12f, 0.5f, 1, " dB"))
    add(PItem.Slider("动态范围", "widthDb", 15f, 60f, 1f, 0, " dB"))
    add(PItem.Slider("频谱平衡", "balance", 0f, 1f, 0.02f, 0, "%", 100f))
    add(PItem.Select("电平映射", "mapMode", listOf("每段自适应", "共用窗口")))
    add(PItem.Select("频率轴", "bandMode", listOf("等宽 0–16k", "等比 30–16k")))
    add(PItem.Slider("总电平", "level", 0.05f, 1.0f, 0.01f, 2))
    add(PItem.Slider("对比度", "contrast", 1f, 3f, 0.1f, 1, "×"))
    add(PItem.Slider("常态分位", "loPct", 0.02f, 0.5f, 0.01f, 0, "%", 100f))
    add(PItem.Slider("峰值分位", "hiPct", 0.8f, 1.0f, 0.01f, 0, "%", 100f))
    add(PItem.Slider("跟踪窗口", "rpWinSec", 3f, 30f, 1f, 0, " 秒"))
    add(
        PItem.Hint(
            listOf(
                "「每段自适应」下：顶满就调低「总电平」，鼓点不够跳就加大「对比度」。",
            )
        )
    )

    /* ==================== 恢复默认设置 ==================== */
    /* 参考实现把这个按钮放在参数页的**标题行**里（`桥接层` 1100 行），
       Windows 版放在「预设」区之前 —— 两边都是「整页最靠前」的位置。
       所以这里的顺序是：音频响应校准 → 恢复默认设置 →（预设区）→ 外观 → …
       ⚠ 它**不重置**「音频输入」：那是用户对这台机器的选择，不是参考实现参数。
         这条语义逐字照抄 Windows 版 `panelNotify(ID_PANEL_RESET)`。 */
    add(PItem.Button("恢复默认设置", BtnId.RESET))
    add(
        PItem.Hint(
            listOf(
                "只把参数回到出厂默认值，保存过的预设一份都不动；",
                "「音频输入」也保持不变（它记的是你对这台机器的选择）。",
            )
        )
    )
}

/** 面板后半段：外观 / 音频响应 / 空闲波浪 / 性能。装配时排在预设区**之后**。 */
val PANEL_TAIL: List<PItem> = buildList {

    /* ==================== 外观 ==================== */
    add(PItem.Sec("外观"))
    add(PItem.Select("光柱风格", "styleMode", listOf("光柱上下跳动", "常驻光柱 · 节拍闪色")))
    add(PItem.Select("颜色主题", "themeIndex", themeOptions()))
    add(PItem.Slider("轮询间隔", "themeCycleSec", 10f, 300f, 10f, 0, " 秒"))
    add(PItem.Slider("光柱离边距离", "beamInset", -0.15f, 0.30f, 0.01f, 2))
    add(PItem.Slider("光柱实心度", "alphaK", 0f, 1f, 0.05f, 2))
    add(
        PItem.Hint(
            listOf(
                "「光柱实心度」0 = 纯加光（像真光打在屏幕上）；",
                "1 = 柱心实心、会遮挡下层。默认 0.10。",
            )
        )
    )

    /* ==================== 音频响应 ==================== */
    add(PItem.Sec("音频响应"))
    add(PItem.Slider("音频响应强度", "audioIntensity", 0.3f, 2.5f, 0.1f, 1))
    add(PItem.Slider("响应范围", "responseRange", 0.3f, 2.0f, 0.1f, 1))
    add(PItem.Switch("启用强调色", "peakEnabled"))
    add(PItem.Slider("强调色强度", "peakIntensity", 0f, 2f, 0.1f, 1))

    /* ==================== 空闲波浪 ==================== */
    add(PItem.Sec("空闲波浪"))
    add(PItem.Switch("启用空闲波浪", "idleEnabled"))
    add(PItem.Slider("防抖", "idleDebounce", 0.5f, 5f, 0.5f, 1, " 秒"))
    add(PItem.Slider("过渡", "idleFade", 0.5f, 5f, 0.5f, 1, " 秒"))

    /* ==================== 性能 ==================== */
    add(PItem.Sec("性能"))
    add(
        PItem.Select(
            "渲染帧率", "fpsLimit",
            listOf("30 fps", "60 fps", "120 fps", "144 fps"),
            listOf(30.0, 60.0, 120.0, 144.0),
        )
    )
    add(
        PItem.Select(
            "音频分析频率", "anaHz",
            listOf("30 Hz", "60 Hz", "120 Hz", "250 Hz（桌面版实测）"),
            listOf(30.0, 60.0, 120.0, 250.0),
        )
    )
    add(
        PItem.Hint(
            listOf(
                "★ 这三层各有自己的时钟，别设成同一个值就以为对齐了：",
                "  频谱 30Hz（固定）→ 分析层 = 上面这项 → 渲染层 = 帧率。",
                "「250 Hz」是桌面版实测值，改小会让包络变慢、鼓点被抹平。",
                "60 fps = 参考实现默认值；更高只是更顺，幅度不变。",
            )
        )
    )
}

/**
 * 完整清单 = 前半段 + 后半段。
 *
 * ⚠ 界面装配**不要**直接用它 —— 预设区要插在两段中间（见 [PANEL_HEAD] 的注释）。
 *   保留它只是为了「一眼看到全部项」和自检遍历。
 */
val PANEL: List<PItem> = PANEL_HEAD + PANEL_TAIL

/** 下拉的索引 ↔ 实际值映射（这几项存的是索引，不是数值本身）。 */
object SelMap {
    val FPS = listOf(30, 60, 120, 144)
    val ANA = listOf(30, 60, 120, 250)

    fun fpsIndex(v: Int) = FPS.indexOf(v).let { if (it < 0) 1 else it }
    fun anaIndex(v: Int) = ANA.indexOf(v).let { if (it < 0) 3 else it }
}
