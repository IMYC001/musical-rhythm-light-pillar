package com.lightpillar

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.SwitchCompat
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * 把 [PANEL] 那份项定义变成真正的 Android 控件。
 *
 * 为什么用原生控件而不是像 Windows 版那样自绘：
 *   滑杆的拖拽惯性、开关的动画、下拉的弹窗、无障碍朗读、深色主题适配……
 *   这些在原生控件里全是白送的。Windows 版那 1264 行自绘面板踩过的坑
 *   （拖拽抽搐、命中测试、非模态输入框）在安卓上一个都不存在。
 */
class PanelBuilder(
    private val act: Activity,
    private val store: SettingsStore,
    private val onChanged: () -> Unit,
) {
    private val ui = Handler(Looper.getMainLooper())
    private var saveTask: Runnable? = null

    /**
     * ★★ 每个控件的「重新从 store 读一遍」动作。
     *
     * 为什么必须有这个：控件原本只在 [build] 时读一次初值，之后只有用户操作才会改显示。
     * 于是「恢复默认设置」把 store 改回默认值之后，界面**还停在旧值上** ——
     * 用户看到的是「点了没反应」，而实际参数早就变了。这是最坏的一种 bug：
     * 界面在说谎，用户只能靠反复重启来确认到底哪个是真的。
     *
     * 做法：每个控件在构造时把自己的同步动作登记进来，[refreshAll] 顺序跑一遍。
     */
    private val refreshers = ArrayList<() -> Unit>()

    /**
     * 按钮动作的回调，由 MainActivity 装配 ——
     * 面板只管把按钮画出来，**不关心按下去要做什么**（动作属于业务，不属于布局）。
     */
    var onButton: (PItem.Button) -> Unit = {}

    /** 改完参数延迟落盘：拖滑杆时每秒会触发几十次，每次都写文件没必要。 */
    private fun scheduleSave() {
        saveTask?.let { ui.removeCallbacks(it) }
        val r = Runnable {
            store.saveSettings()
            onChanged()
        }
        saveTask = r
        ui.postDelayed(r, 350)
    }

    /**
     * 把 [items] 逐个变成控件加进 [root]。
     *
     * ⚠ 默认参数是整份 [PANEL]，但 MainActivity **不用**默认值 ——
     *   它分两次调用（[PANEL_HEAD] / [PANEL_TAIL]），中间夹着预设区。
     */
    fun build(root: LinearLayout, items: List<PItem> = PANEL) {
        for (item in items) {
            when (item) {
                is PItem.Sec -> root.addView(sectionView(item.title))
                is PItem.Hint -> root.addView(hintView(item.lines))
                is PItem.Slider -> root.addView(sliderView(item))
                is PItem.Switch -> root.addView(switchView(item))
                is PItem.Select -> root.addView(selectView(item))
                is PItem.Button -> root.addView(buttonView(item))
            }
        }
    }

    /** 把所有控件刷回 store 里的当前值。见 [refreshers]。 */
    fun refreshAll() {
        for (r in refreshers) r()
    }

    /* ==================== 给 MainActivity 用的装配件 ==================== */

    /* 「音频来源」那一整段是安卓特有的，不在 [PANEL] 清单里，
       由 MainActivity 单独摆。把这两个工厂开出去，免得标题/说明各写一套样式
       （以前就是因为标题留在清单里、控件在外面加，标题画到了控件下面）。 */
    fun sectionHeader(t: String): View = sectionView(t)
    fun hintBlock(lines: List<String>): View = hintView(lines)

    /* ==================== 基础控件 ==================== */

    private fun sectionView(t: String): TextView = TextView(act).apply {
        text = t
        setTextColor(Color.parseColor("#C9B6FF"))
        textSize = 13f
        typeface = Typeface.DEFAULT_BOLD
        setPadding(0, dp(20), 0, dp(6))
    }

    private fun hintView(lines: List<String>): TextView = TextView(act).apply {
        text = lines.joinToString("\n")
        setTextColor(Color.parseColor("#8A8A99"))
        textSize = 11.5f
        setPadding(0, dp(2), 0, dp(6))
        setLineSpacing(dp(3).toFloat(), 1f)
    }

    private fun rowLabel(t: String): TextView = TextView(act).apply {
        text = t
        setTextColor(Color.parseColor("#E8E8F0"))
        textSize = 14f
    }

    private fun dp(v: Int): Int = (v * act.resources.displayMetrics.density).roundToInt()

    /** 每一项都放进一个白底卡片式的容器里，深色主题下层次清楚。 */
    private fun card(child: View): LinearLayout = LinearLayout(act).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(14), dp(10), dp(14), dp(10))
        addView(child)
    }

    /* ==================== 滑杆 ==================== */

    /* ⚠ 参数名不能叫 `it`：下面 `setOnClickListener { }` 这类 lambda 里
       `it` 会被 lambda 自己的隐式参数遮蔽，于是 `it.label` 解析不到 —— 报的是
       "Unresolved reference: label"，看着莫名其妙。统一叫 spec。 */
    private fun sliderView(spec: PItem.Slider): View {
        val acc = store.settings.byName[spec.key]
        val steps = ((spec.hi - spec.lo) / spec.step).roundToInt().coerceAtLeast(1)

        /* ⚠ KeyAcc.get/set 是**函数类型的属性**，acc 又是可空的，
           所以必须写 `acc?.get?.invoke()`；直接 `acc?.get()` 编译器会说
           "Reference has a nullable type '() -> Double'"。 */
        fun cur(): Double = acc?.get?.invoke() ?: 0.0
        fun put(v: Double) {
            acc?.set?.invoke(v)
        }

        val head = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL }
        val name = rowLabel(spec.label)
        val valTv = TextView(act).apply {
            setTextColor(Color.parseColor("#9FE1CB"))
            textSize = 14f
            gravity = Gravity.END
        }
        head.addView(name, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(valTv)

        fun fmt(v: Float): String {
            val shown = v * spec.mul
            return if (spec.dec <= 0) "${shown.roundToInt()}${spec.unit}"
            else String.format(java.util.Locale.US, "%.${spec.dec}f${spec.unit}", shown)
        }

        val bar = SeekBar(act).apply {
            max = steps
            progress = ((cur().toFloat() - spec.lo) / spec.step).roundToInt().coerceIn(0, steps)
        }

        /* ⚠ 刷新界面时**不能**把值写回 store。SeekBar 只能停在步长的整数倍上：
           「频谱平衡」默认 0.85、步长 0.02 → 42.5 档，会被圆成 42 档 = 0.84，
           写回去就把「恢复默认」的结果改掉了一点（只差 0.01，但自检会当场判成不一致，
           而且用户下一次拖这根滑杆时会发现起点不在他以为的位置）。 */
        var suppress = false

        bar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                val v = spec.lo + p * spec.step
                if (!suppress) put(v.toDouble())
                valTv.text = fmt(v)
                if (fromUser) scheduleSave()
            }

            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        })

        /* 数值文本按 store 的**真值**显示，不按 bar 的档位 ——
           否则 0.85 会被显示成 84%（就是上面那条注释里的同一个圆整问题）。 */
        valTv.text = fmt(cur().toFloat())
        refreshers.add {
            suppress = true
            bar.progress = ((cur().toFloat() - spec.lo) / spec.step).roundToInt().coerceIn(0, steps)
            suppress = false
            valTv.text = fmt(cur().toFloat())
        }

        val box = LinearLayout(act).apply {
            orientation = LinearLayout.VERTICAL
            addView(head)
            addView(bar)
        }
        return card(box)
    }

    /* ==================== 开关 ==================== */

    private fun switchView(spec: PItem.Switch): View {
        val acc = store.settings.byName[spec.key]

        /* 同 [sliderView]：刷新界面时屏蔽回调，免得把「读出来的值」当成「用户拨的值」
           再写回 store（值一样，但会顺带触发一次落盘）。 */
        var suppress = false

        val sw = SwitchCompat(act).apply {
            text = spec.label
            setTextColor(Color.parseColor("#E8E8F0"))
            textSize = 14f
            isChecked = (acc?.get?.invoke() ?: 0.0) != 0.0
            setOnCheckedChangeListener { _, checked ->
                if (suppress) return@setOnCheckedChangeListener
                acc?.set?.invoke(if (checked) 1.0 else 0.0)
                scheduleSave()
            }
        }
        refreshers.add {
            suppress = true
            sw.isChecked = (acc?.get?.invoke() ?: 0.0) != 0.0
            suppress = false
        }
        return card(sw)
    }

    /* ==================== 下拉 ==================== */

    private fun selectView(spec: PItem.Select): View {
        val acc = store.settings.byName[spec.key]
        val head = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL }
        val name = rowLabel(spec.label)
        val curTv = TextView(act).apply {
            setTextColor(Color.parseColor("#9FE1CB"))
            textSize = 14f
            gravity = Gravity.END
        }

        /**
         * 当前值 → 选项下标。
         *
         * ⚠ `themeIndex` 必须特判：它的选项表多一项「自动轮询」占位，
         *   而占位对应的不是「值 = 0」而是「`themeAuto = true`」。
         *   Windows 版：`g_themeSel = themeAuto ? 0 : (themeIndex + 1)`。
         *   不特判的后果是**整体错位一格** —— 选「霁紫」渲染出「沧蓝」。
         */
        fun indexOfCurrent(): Int {
            if (spec.key == "themeIndex") {
                val s = store.settings
                return if (s.themeAuto) 0 else (s.themeIndex + 1).coerceIn(1, spec.opts.size - 1)
            }
            val v = acc?.get?.invoke() ?: 0.0
            val vals = spec.vals
            if (vals == null) return v.toInt().coerceIn(0, spec.opts.size - 1)
            /* ⚠ 显式写参数名 x：别用隐式 it，容易和外层的 it 混。 */
            val k = vals.indexOfFirst { x -> abs(x - v) < 1e-3 }
            return if (k < 0) 0 else k
        }

        fun sync() {
            curTv.text = spec.opts.getOrElse(indexOfCurrent()) { "?" }
        }
        sync()
        refreshers.add { sync() }

        head.addView(name, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(curTv)

        val box = LinearLayout(act).apply {
            orientation = LinearLayout.VERTICAL
            addView(head)
            isClickable = true
            setOnClickListener {
                AlertDialog.Builder(act)
                    .setTitle(spec.label)
                    .setSingleChoiceItems(spec.opts.toTypedArray(), indexOfCurrent()) { d, which ->
                        if (spec.key == "themeIndex") {
                            /* 见 [applyThemeSemantics]：一次点击要同时改两个键
                               （themeAuto + themeIndex），并且 0 号项是「自动」而不是「主题 0」。 */
                            applyThemeSemantics(which)
                        } else {
                            acc?.set?.invoke(spec.vals?.getOrNull(which) ?: which.toDouble())
                        }
                        sync()
                        scheduleSave()
                        d.dismiss()
                    }
                    .setNegativeButton("取消", null)
                    .show()
            }
        }
        return card(box)
    }

    /* ==================== 按钮 ==================== */

    /**
     * 一个纯动作按钮。目前只有「恢复默认设置」
     * （原作品参数页标题行里那个按钮 / Windows 版的 `ID_PANEL_RESET`）。
     *
     * ★ 刻意**不做二次确认**，和原作品、Windows 版都一致。理由不是偷懒：
     *   恢复默认**不碰预设**（预设存在 presets.ini，和 settings.ini 是两份文件），
     *   自己存过的档位一个都不会丢 —— 所以不存在「一按全没了」这种不可逆后果。
     *   多一层弹窗反而会让用户以为这是个危险操作。
     *
     * 宽度铺满（MATCH_PARENT）：原作品里它是标题行右侧的一个小按钮，
     * 但安卓的标题是独立一行、没有「行内放按钮」的位置，铺满更好点。
     */
    private fun buttonView(spec: PItem.Button): View = Button(act).apply {
        text = spec.label
        textSize = 14f
        setOnClickListener { onButton(spec) }
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply {
            topMargin = dp(4)
            bottomMargin = dp(2)
        }
    }

    /* ==================== 主题下拉（带「自动轮询」联动） ==================== */

    /**
     * 主题下拉的一次选择 → 两个键。
     *
     * ★ 逐字照抄 Windows 版 `main.cpp` 里 `S_THEME` 的处理：
     *   ```
     *   if (sel <= 0) { themeAuto = true; }                          // 第 0 项 = 自动轮询
     *   else { themeAuto = false; themeIndex = sel - 1; themeClock = 0; }
     *   ```
     *   · `themeIndex` 是 **0 基**，而下拉第 0 项被「自动轮询」占了 ⇒ 值 = `sel - 1`；
     *   · 选「自动轮询」时**不动** `themeIndex` —— 它保留当前正在显示的那一套，
     *     然后由渲染线程继续往下轮。
     *
     * ⚠ 这个方法以前存在但**从来没被调用过**（死代码），这就是「选主题错位一格」
     *   和「自动轮询不工作」两个症状的共同来源。
     *
     * @param sel 下拉里选中的下标（0 = 自动轮询，1..N = LP_THEMES[sel-1]）
     */
    fun applyThemeSemantics(sel: Int) {
        val s = store.settings
        if (sel <= 0) {
            s.themeAuto = true
        } else {
            s.themeAuto = false
            s.themeIndex = sel - 1
        }
        s.clamp()
    }

    /** 浮点比较用（自检/预设诚实性判断）。 */
    fun nearly(a: Float, b: Float) = abs(a - b) < 1e-4f
}
