package com.lightpillar

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import android.graphics.Color
import kotlin.math.roundToInt

/**
 * 设置页。同时负责三件权限事：
 *   ① 悬浮窗（SYSTEM_ALERT_WINDOW）—— 没有它光柱根本叠不上去
 *   ② 麦克风（RECORD_AUDIO）—— 运行时权限
 *   ③ MediaProjection —— 只有选了「内部音频」才要，且每次启动都要重新授权
 */
class MainActivity : AppCompatActivity() {

    private lateinit var store: SettingsStore
    private lateinit var builder: PanelBuilder
    private lateinit var presetPanel: PresetPanel
    private lateinit var status: TextView
    private lateinit var startBtn: Button

    private val overlayReq =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { refresh() }

    private val permReq =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { refresh() }

    private val projReq =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { r ->
            if (r.resultCode == RESULT_OK && r.data != null) {
                /* ★★★ 这里**只**把原始授权结果 (resultCode, data) 交给服务，
                   绝不在这里调 getMediaProjection()。
                   原因（真机踩过、表现是「一点内部音频就闪退」）：
                     Android 10+ 要求取 MediaProjection 时，调用方**此刻**必须已经有
                     一个 foregroundServiceType=mediaProjection 的前台服务在跑，
                     否则抛 SecurityException。而它抛在 onActivityResult 里，
                     连 toast 都来不及弹，用户看到的就是闪退。
                   正确顺序：先起服务 → 服务 startForeground(mediaProjection) →
                     **然后**服务自己 getMediaProjection()。见 [ProjectionHolder]。 */
                ProjectionHolder.setResult(r.resultCode, r.data!!)
                startOverlay()
            } else {
                toast("没授权 → 内部音频用不了。可以先用「麦克风」。")
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        /* ★★★ 和 OverlayService **共用同一个** SettingsStore（见 [SettingsHolder]）。
           以前这里 `SettingsStore(this).bootstrap()`，服务那边也各来一份 ——
           面板改的是这一份、渲染线程读的是服务那一份启动快照，
           于是「改参数要停掉重开才生效」。共用之后下一帧就生效。 */
        store = SettingsHolder.get(this)

        /* ★ 自检开关：settings.ini 的 `[android] debugMode = 100`。
           放在 bootstrap() 之后、建界面之前 ——
             · 必须在 bootstrap 之后：bootstrap 本身是自检要验的一环；
             · 必须在建界面之前：报告要在任何 UI 交互前就落盘，
               否则 adb 那边刚启动就来取文件会取到上一份。
           全程用 cacheDir/selftest/ 的临时文件，**不碰**用户真实配置。 */
        if (store.settings.debugMode == 100) {
            val (pass, total) = SelfCheck.runAndReport(this)
            Toast.makeText(this, "自检 $pass/$total 通过 → files/自检报告.json", Toast.LENGTH_LONG).show()
        }

        setContentView(R.layout.activity_main)

        val root = findViewById<LinearLayout>(R.id.root)

        /* ★ 面板构造器要**早于**「音频来源」那一段建好 ——
           那一段的标题和说明是借它的样式工厂画的（见下）。 */
        builder = PanelBuilder(this, store) { }

        /* ---- 顶部状态 + 启停 ---- */
        status = TextView(this).apply {
            setTextColor(Color.parseColor("#B4B2A9"))
            textSize = 12f
            setPadding(0, 0, 0, dp(10))
        }
        root.addView(status)

        startBtn = Button(this).apply {
            setOnClickListener { toggle() }
        }
        root.addView(startBtn)

        val permBtn = Button(this).apply {
            text = "检查权限"
            setOnClickListener { askAll() }
        }
        root.addView(permBtn)

        /* ---- 音频来源（安卓特有，不在 27 键清单里，所以单独做） ----
           ⚠ 分组标题必须在控件**之前**。这一段以前是拆成两半摆的：标题留在
             [PANEL] 里、下拉在这里加，而面板是在这个下拉之后才 build 的 ——
             结果「音频来源」这个标题画到了「音频输入」那一行的**下面**。
             现在整段一起摆，顺序写死在这里。 */
        root.addView(builder.sectionHeader("音频来源"))
        root.addView(audioSourcePicker())
        root.addView(
            builder.hintBlock(
                listOf(
                    "麦克风：零摩擦，放音乐时手机外放 → 麦克风听到 → 光柱动。",
                    "内部音频：抓别的 App 放的声音，每次启动要授权一次；",
                    "  视频 App / 通话 / 系统提示音采不到 —— 播放方可以拒绝被采集。",
                )
            )
        )

        /* ---- 参数面板 ----
           ⚠ 第三个参数（onChanged）现在是**空**的，这不是漏写：
              面板的滑杆回调直接写进 store，而渲染线程每帧都读同一个 store，
              所以下一帧就生效，不需要任何「通知服务」的动作。
              以前这里传的是 syncService()（重发 ACTION_START 让服务重新
              bootstrap），那个做法本身就是「实时生效」失效的原因之一：
              服务的 onStartCommand 在 overlay != null 时什么都不做 ——
              两边都以为对方会做，结果没人做。详见 [SettingsHolder]。 */
        presetPanel = PresetPanel(this, store, root) { }

        /* 按钮动作在这里装配：面板只负责把按钮画出来，按下去做什么属于业务。 */
        builder.onButton = { btn -> if (btn.id == BtnId.RESET) resetAll() }

        /* ★ 顺序照参考实现 / Windows 版：
             参数分组 → 「恢复默认设置」 → 预设区 → 其余分组。
           所以清单被切成两段，预设区插在中间（见 [PANEL_HEAD] 的注释）。 */
        builder.build(root, PANEL_HEAD)
        presetPanel.build()
        builder.build(root, PANEL_TAIL)

        /* ---- 底部提示 ---- */
        root.addView(
            TextView(this).apply {
                text = "改完参数立即生效，并自动保存。要停掉光柱，点上面的「停止光柱」" +
                    "或在通知栏点「停止」。"
                setTextColor(Color.parseColor("#6E6E7A"))
                textSize = 11.5f
                setPadding(0, dp(18), 0, dp(30))
            }
        )
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).roundToInt()

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()

    /* ==================== 状态刷新 ==================== */

    private fun refresh() {
        val hasOverlay = canOverlay()
        val hasMic = ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

        val sb = StringBuilder()
        sb.append(if (hasOverlay) "✓ 悬浮窗权限" else "✗ 悬浮窗权限（必需）")
        sb.append("　")
        sb.append(if (hasMic) "✓ 麦克风" else "✗ 麦克风")
        sb.append("\n")
        sb.append(if (OverlayService.alive) "● 光柱运行中" else "○ 未启动")
        status.text = sb.toString()

        startBtn.text = if (OverlayService.alive) "停止光柱" else "启动光柱"
    }

    private fun canOverlay(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) Settings.canDrawOverlays(this)
        else true

    /* ==================== 恢复默认设置 ==================== */

    /**
     * 「恢复默认设置」按钮。逐条对齐两个上游实现：
     *   · 参考实现 `桥接层` 1100~1116 行（`values = defaults()` +
     *     `applyAudio(audioDefaults())` + `activePreset = null` + 刷新两处 UI）；
     *   · Windows 版 `main.cpp` 的 `panelNotify(ID_PANEL_RESET)`。
     *
     * ★ 有两个键**故意不重置**：
     *   `audioSourceIndex`（= Windows 版的 `audioIdleMode`）和 `debugMode`
     *   （= `displayIndex` 那一类）。它们不是参考实现参数，而是「用户对**这台机器**
     *   的选择」。Windows 版那里的注释原话：
     *     「『恢复默认』不该把『声音来源 / 显示在哪块屏』一起改掉」。
     *   在安卓上后果更明显：顺手把音频输入从「内部音频」改回「麦克风」，
     *   用户会以为程序坏了，还得重新去授权一次。
     *
     * ★ 预设**一份都不动**。预设存在 `presets.ini`，和 `settings.ini` 是两份文件，
     *   所以「恢复默认」是可逆的 —— 存过档的人随时能切回去。
     *   参考实现的提示语也特意把这一点写出来（「保存过的预设不受影响」），
     *   因为「预设会不会一起没」是用户按下这个按钮前唯一会犹豫的事。
     *
     * ⚠ 三件容易漏掉的事，缺一件用户就会觉得「没生效」或「出 bug 了」：
     *   ① 面板所有控件要刷回默认值（[PanelBuilder.refreshAll]）；
     *   ② 预设区的「当前档位」要变成「（未选择）」（[PresetPanel.refresh]）；
     *   ③ 必须 `saveSettings()` 落盘 —— 否则重启后旧值又回来了。
     */
    private fun resetAll() {
        val s = store.settings
        val keepSource = s.audioSourceIndex
        val keepDebug = s.debugMode

        s.resetToDefaults()
        s.audioSourceIndex = keepSource
        s.debugMode = keepDebug
        s.clamp()

        /* 已经离开任何一套预设了，下拉不该还停在上面。 */
        store.selected = 0
        store.saveSettings()

        builder.refreshAll()
        presetPanel.refresh()

        toast("已恢复默认设置（参数与校准一起；音频输入不变，保存过的预设不受影响）")
    }

    /**
     * 音频来源下拉。
     *
     * 这是**安卓特有**的参数（Windows 版只有 WASAPI loopback 一种来源，没得选），
     * 所以刻意不进 27 键清单 —— 那份清单是两个平台逐键对照的基准，
     * 多一个键对照结果就全是假差异。
     *
     * ⚠ 这里**只有两档**。曾经有第三档「合成测试音」（内部合成一个脉冲信号，
     *   不需要任何权限），**已按要求删除**。要让画面动起来做诊断时改用
     *   `[android] debugMode = 12/13/14` —— 那是固定 drive，不依赖音频链路，
     *   比测试音更确定。
     */
    private fun audioSourcePicker(): View {
        val labels = listOf(
            "麦克风（外放时能听到音乐）",
            "内部音频（Android 10+，需授权）",
        )

        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val name = TextView(this).apply {
            text = "音频输入"
            setTextColor(Color.parseColor("#E8E8F0"))
            textSize = 14f
        }
        val cur = TextView(this).apply {
            setTextColor(Color.parseColor("#9FE1CB"))
            textSize = 14f
            gravity = Gravity.END
        }

        fun sync() {
            cur.text = labels[store.settings.audioSourceIndex.coerceIn(0, labels.size - 1)]
        }
        sync()

        row.addView(name, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(cur)

        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14), dp(10), dp(14), dp(10))
            addView(row)
            isClickable = true
            setOnClickListener {
                AlertDialog.Builder(this@MainActivity)
                    .setTitle("音频输入")
                    .setSingleChoiceItems(
                        labels.toTypedArray(),
                        store.settings.audioSourceIndex.coerceIn(0, labels.size - 1)
                    ) { d, which ->
                        store.settings.audioSourceIndex = which
                        store.saveSettings()
                        sync()
                        d.dismiss()
                        restartOverlay()
                    }
                    .setNegativeButton("取消", null)
                    .show()
            }
        }
        return box
    }

    /** 换音频来源必须重建整条链路：停服务 → 重新授权（如果是内部音频）→ 再起。 */
    private fun restartOverlay() {
        if (!OverlayService.alive) return
        stopService(Intent(this, OverlayService::class.java))
        startBtn.postDelayed({
            if (store.settings.audioSourceIndex == 1) {
                val mpm = getSystemService(MediaProjectionManager::class.java)
                projReq.launch(mpm.createScreenCaptureIntent())
            } else {
                startOverlay()
            }
        }, 450)
    }

    /* ==================== 权限 ==================== */

    private fun askAll() {
        if (!canOverlay()) {
            startActivityForResult(
                Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:$packageName")
                ), 1001
            )
            toast("请打开「允许显示在其他应用上层」")
            return
        }
        val want = mutableListOf<String>()
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED
        ) want.add(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, "android.permission.POST_NOTIFICATIONS") !=
            PackageManager.PERMISSION_GRANTED
        ) want.add("android.permission.POST_NOTIFICATIONS")

        if (want.isEmpty()) {
            toast("权限都齐了")
            refresh()
        } else {
            permReq.launch(want.toTypedArray())
        }
    }

    /* ==================== 启停 ==================== */

    private fun toggle() {
        if (OverlayService.alive) {
            stopService(Intent(this, OverlayService::class.java))
            startBtn.postDelayed({ refresh() }, 300)
            return
        }
        if (!canOverlay()) {
            askAll()
            return
        }
        val s = store.settings
        if (s.audioSourceIndex == 1) {
            /* 内部音频每次都要重新授权（系统规定），先弹授权再起服务。 */
            val mpm = getSystemService(MediaProjectionManager::class.java)
            projReq.launch(mpm.createScreenCaptureIntent())
        } else {
            startOverlay()
        }
    }

    private fun startOverlay() {
        val i = Intent(this, OverlayService::class.java)
            .setAction(OverlayService.ACTION_START)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(i)
        else startService(i)
        startBtn.postDelayed({ refresh() }, 500)
    }

    /** 面板改完参数后，让正在跑的服务同步一份。
     *
     * ★★★ 现在**不需要做任何事**了 —— 保留这个函数只为说明清楚，
     *     免得后来人以为「漏了一步」又把它加回来。
     *
     * 以前的做法是「重发一次 ACTION_START，让服务重新 bootstrap」。
     * 那是错的，而且正是「改参数没反应、得停掉重开」的成因：
     *   · 服务 `onStartCommand` 里 `overlay != null` 时什么都不做 ——
     *     它**以为**参数是 Activity 推过来的，Activity **以为**服务会自己去读；
     *   · 两边都以为对方会做，结果没人做。
     *
     * 现在的模型干净得多：Activity 和服务**共用同一个 SettingsStore**
     * （[SettingsHolder]），面板的滑杆回调直接写进这个对象，
     * 渲染线程本来就每帧 `store.settings` —— 下一帧就是新值。
     * 分析链需要重建的那几项由渲染线程每帧的 `syncAnalyzer()` 判断，见 [OverlayService]。
     */
    @Suppress("unused")
    private fun syncService() {
        /* 空实现。见上。 */
    }

    @Deprecated("悬浮窗授权没有 result 回调，只能 onResume 里查")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == 1001) refresh()
    }
}
