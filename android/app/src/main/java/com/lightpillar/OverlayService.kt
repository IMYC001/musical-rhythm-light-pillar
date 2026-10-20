package com.lightpillar

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import java.io.File
import kotlin.math.max
import kotlin.math.min

/**
 * 覆盖层前台服务：一块**全屏透明**的悬浮窗，画那两根光柱。
 *
 * ── 为什么必须是悬浮窗，不能是动态壁纸 ─────────────────────────────
 * 你的需求是「不要把黑色背景照搬，只要那两根光柱叠在桌面上」。
 * 安卓上只有悬浮窗能做到：
 *   · 动态壁纸的 Surface **不透明**（底下是黑的，不是你的桌面），
 *     它只能是「一张会动的全屏壁纸」，做不到「光叠在桌面上」；
 *   · 悬浮窗 + `PixelFormat.TRANSLUCENT` + 不画背景 = 真的只有两根光柱。
 *
 * ── 触摸穿透 ───────────────────────────────────────────────────────
 * 覆盖层窗口带 `FLAG_NOT_TOUCHABLE`，触摸直接落到下层应用。
 * 代价是它自己收不到任何触摸 —— 所以**调参数的入口是打开本 App 的图标**
 * （设置页）。
 *
 * ⚠ 这里曾经还有一个「白色半透明圆点」悬浮球，作为第二个调参入口。
 *   **已按要求删除**。删掉之后：屏幕上只剩光柱，一个像素的多余元素都没有；
 *   调参走 App 图标。覆盖层本身依旧是穿透的，不影响桌面点击。
 */
class OverlayService : Service() {

    companion object {
        private const val TAG = "LightPillar"
        private const val CH_ID = "lightpillar_overlay"
        private const val NOTI_ID = 1
        const val ACTION_START = "com.lightpillar.START"
        const val ACTION_STOP = "com.lightpillar.STOP"

        @Volatile var alive = false
            private set
    }

    private lateinit var store: SettingsStore
    private lateinit var audio: AudioEngine
    private lateinit var analyzer: Analyzer
    private var wm: WindowManager? = null

    private var overlay: SurfaceView? = null
    private var render: RenderThread? = null

    override fun onCreate() {
        super.onCreate()
        /* ★★★ 和 MainActivity **共用同一个** SettingsStore（见 SettingsHolder）。
           以前这里各自 new 一份，于是面板改的是 Activity 那份、渲染线程读的是
           这份启动快照 —— 「改参数没反应，得停掉重开」。共用之后实时生效是自然结果。 */
        store = SettingsHolder.get(this)
        audio = AudioEngine()
        analyzer = Analyzer()
        wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        alive = true
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        startForegroundCompat()

        /* ★★★ 顺序：必须**先** startForegroundCompat()（里面按 mediaProjection 类型起前台），
           **再**取 MediaProjection。反过来就是 SecurityException 闪退。
           详见 [ProjectionHolder] 的注释。 */
        val gotProjection = if (ProjectionHolder.hasResult()) obtainProjection() else false

        if (overlay == null) {
            addOverlay()
            startAudio()
        } else if (gotProjection) {
            /* 服务已经在跑、只是刚拿到内部音频的授权 → 重建音频链路。
               （audio.start() 内部会先 stop()，不用在这里手动停。） */
            startAudio()
        }
        return START_STICKY
    }

    /**
     * 在前台服务就绪之后取 MediaProjection。
     *
     * @return 真的拿到了返回 true
     */
    private fun obtainProjection(): Boolean {
        val r = ProjectionHolder.takeResult() ?: return false
        return try {
            val mpm = getSystemService(MediaProjectionManager::class.java)
            val proj = mpm.getMediaProjection(r.first, r.second)
            if (proj == null) {
                Log.e(TAG, "getMediaProjection 返回 null")
                false
            } else {
                /* ★ 注册回调：不注册的话 Android 14+ 在创建虚拟显示时会抛
                   IllegalStateException；而且用户从系统状态栏「停止投放」时
                   我们能收到通知，好把音频链路一起停掉，不留一个空转的采集。 */
                proj.registerCallback(
                    object : MediaProjection.Callback() {
                        override fun onStop() {
                            Log.i(TAG, "MediaProjection 被停止（用户或系统）")
                            ProjectionHolder.set(null)
                        }
                    },
                    Handler(Looper.getMainLooper())
                )
                ProjectionHolder.set(proj)
                Log.i(TAG, "MediaProjection 已取得（前台服务已就绪）")
                true
            }
        } catch (e: Exception) {
            Log.e(TAG, "取 MediaProjection 失败", e)
            false
        }
    }

    override fun onDestroy() {
        alive = false
        render?.stopLoop()
        render = null
        audio.stop()
        /* 不 release 的话系统状态栏会一直挂着「正在录制或投放」的提示 */
        ProjectionHolder.release()
        try {
            overlay?.let { wm?.removeView(it) }
        } catch (_: Exception) {
        }
        overlay = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    /* ==================== 前台通知 ==================== */

    private fun startForegroundCompat() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(CH_ID, getString(R.string.noti_channel), NotificationManager.IMPORTANCE_LOW)
            ch.setShowBadge(false)
            nm.createNotificationChannel(ch)
        }

        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, OverlayService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        val noti: Notification = Notification.Builder(this, CH_ID)
            .setContentTitle(getString(R.string.noti_title))
            .setContentText(getString(R.string.noti_text))
            .setSmallIcon(android.R.drawable.ic_menu_gallery)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, getString(R.string.noti_stop), stop).build())
            .setOngoing(true)
            .build()

        /* ⚠ Android 14 起必须声明前台服务类型，且要和实际用途匹配。
           这里按音频来源选：麦克风 / 内部音频（MediaProjection）。 */
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val type = if (store.settings.audioSourceIndex == 1)
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            else
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            startForeground(NOTI_ID, noti, type)
        } else {
            startForeground(NOTI_ID, noti)
        }
    }

    /* ==================== 覆盖层窗口 ==================== */

    private fun addOverlay() {
        val sv = SurfaceView(this)
        sv.holder.setFormat(PixelFormat.TRANSLUCENT)
        sv.setZOrderOnTop(true)

        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
            else
                @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_SYSTEM_OVERLAY,
            /* ★ 穿透三件套 + 一个「铺满」用的：
                 NOT_TOUCHABLE    → 触摸落到下层应用（这是「不挡桌面点击」）
                 NOT_FOCUSABLE    → 不抢输入焦点，不弹软键盘
                 LAYOUT_IN_SCREEN → 让窗口按**整屏**布局，而不是「状态栏以下」。
                    ⚠ 实测踩过：只加 LAYOUT_NO_LIMITS 不够 —— 窗口尺寸仍按内容区
                      算出来是 480x610，比屏幕少 30px（正好是状态栏），
                      于是光柱到不了顶边，「铺满两边」就不成立。
                      SF 转储里能看到 pos=(0,30) size=(480,610)，就是这个问题。
                 LAYOUT_NO_LIMITS → 允许继续铺到屏幕外的圆角/挖孔区域 */
            WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT
        )
        lp.gravity = Gravity.TOP or Gravity.START

        sv.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) {
                startRender(h.surface)
            }

            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {
                render?.setSize(w, hh)
            }

            override fun surfaceDestroyed(h: SurfaceHolder) {
                render?.stopLoop()
                render = null
            }
        })

        try {
            wm?.addView(sv, lp)
            overlay = sv
        } catch (e: Exception) {
            Log.e(TAG, "添加覆盖层失败（多半是没有 SYSTEM_ALERT_WINDOW 权限）", e)
        }
    }

    /* ==================== 音频 ==================== */

    fun startAudio() {
        val s = store.settings
        val src = when (s.audioSourceIndex) {
            1 -> AudioEngine.Source.INTERNAL
            else -> AudioEngine.Source.MIC
        }
        val proj = if (src == AudioEngine.Source.INTERNAL) ProjectionHolder.get() else null
        audio.start(src, proj)
        /* 音频链路刚重建（采样率/声道可能变了）→ 走**重档**同步，
           并且把分析时钟归零（新链路的第一个样本要重新开始计时；
           syncAnalyzer 在 doInit 时会自己调 resetAnalysisClock）。 */
        syncAnalyzer(doInit = true)
    }

    /* ---- 分析链参数跟踪：用来判断「这次改动要不要重建频段/直方图」 ---- */

    private var prevBand = -1
    private var prevHz = -1.0
    private var prevWin = -1.0
    private var prevMap = -1
    private var prevLoTrack = -1.0
    private var prevHiTrack = -1.0
    private var prevGain = Double.NaN
    private var prevWidth = Double.NaN
    private var prevBal = Double.NaN

    /**
     * 把设置同步到分析链。
     *
     * ★★★ **这是「参数实时生效」的落地点**：渲染线程每帧都调它（见 [RenderThread.run]），
     *     所以面板里拖一下滑杆，下一帧就反映到画面，不需要停掉覆盖层再重开。
     *     （以前只在 `startAudio()` 里调一次，改参数当然没反应。）
     *
     * ★★★ **必须分两档，不能一律 `paramsChanged()`**（照抄 Windows 版
     *     `pushAudioParams` / `pushAudioParamsLight` 的区分）：
     *
     *   · **重档**（`paramsChanged()` = `buildBands()` + `resetTrackers()`）：
     *     改的是「频段怎么划分 / 分位怎么定义 / 映射方式」这类**结构性**参数。
     *     触发键：`bandMode mapMode loPct hiPct rpWinSec anaHz gainDb widthDb`
     *   · **轻档**（`rebuildTiltOnly()`）：只改**数值大小**，频段划分和滚动直方图都不动。
     *     触发键：`contrast level`（以及每帧都在变的 `intensity/responseRange` 等）
     *
     *   Windows 版的原话：「原来无论改哪个音频参数都调 `paramsChanged()`，
     *   而它会 `resetTrackers()` —— 把 10 秒的滚动直方图整个清空 →
     *   用户拖一下滑杆就重置一次，当然『怎么调都没效果』。」
     *
     * ⚠ 两个容易漏的键，漏了会静默算错：
     *   · `rpWinSec`：不走重档 ⇒ `rpWin` 不变 ⇒ `rpPos % rpWin` 用旧值 ⇒
     *     **环形缓冲被污染**，而且「跟踪窗口」这个控件在说谎。
     *   · `gainDb` / `widthDb`：不走重档 ⇒ `buildBands()` 不跑 ⇒
     *     `lvMin/lvMax` 保持旧值（`rebuildTiltOnly()` 不重算它们）⇒ 宽窗口模式下电平算错。
     *
     * @param doInit true 时无条件走重档（音频链路刚重建，一切从头来）
     */
    fun syncAnalyzer(doInit: Boolean = false) {
        val s = store.settings

        val band = s.bandMode
        val hz = s.anaHz.toDouble()
        val win = s.rpWinSec.toDouble()
        val map = s.mapMode
        val lo = s.loPct.toDouble()
        val hi = s.hiPct.toDouble()
        val gain = s.gainDb.toDouble()
        val width = s.widthDb.toDouble()

        analyzer.gainDb = gain
        analyzer.widthDb = width
        analyzer.balance = s.balance.toDouble()
        analyzer.mapMode = map
        analyzer.bandMode = band
        analyzer.level = s.level.toDouble()
        analyzer.contrast = s.contrast.toDouble()
        analyzer.loPct = lo
        analyzer.hiPct = hi
        analyzer.rpWinSec = win
        analyzer.intensity = s.audioIntensity.toDouble()
        analyzer.responseRange = s.responseRange.toDouble()
        analyzer.anaHz = hz
        analyzer.idleEnabled = s.idleEnabled
        analyzer.idleDebounce = s.idleDebounce.toDouble()
        analyzer.idleFade = s.idleFade.toDouble()
        analyzer.sampleRate = audio.sampleRate.toDouble()
        analyzer.channels = audio.channels

        /* ⚠ 先把「分析频率变了吗」记下来 —— 下面 structural 分支里会把 prevHz
           更新成 hz，之后再比就永远相等了。 */
        val hzChanged = doInit || hz != prevHz

        val structural = doInit ||
            band != prevBand ||
            hz != prevHz ||
            win != prevWin ||
            map != prevMap ||
            lo != prevLoTrack ||
            hi != prevHiTrack ||
            gain != prevGain ||
            width != prevWidth

        if (structural) {
            analyzer.paramsChanged()
            prevBand = band
            prevHz = hz
            prevWin = win
            prevMap = map
            prevLoTrack = lo
            prevHiTrack = hi
            prevGain = gain
            prevWidth = width
            prevBal = s.balance.toDouble()
        } else if (s.balance.toDouble() != prevBal) {
            /* 轻档：只重算倾斜表，**不动**滚动直方图。
               ⚠ 加这个「变了吗」的判断不是洁癖 —— 本函数现在**每帧**都被调，
                 不加的话就是每秒 60~144 次无意义的 64 次 ln 运算。
                 判据只看真正影响倾斜表的量：`balance`（和 bandMode，
                 但那个已经在 structural 里了）。 */
            analyzer.rebuildTiltOnly()
            prevBal = s.balance.toDouble()
        }
        if (hzChanged) analyzer.resetAnalysisClock()
    }

    /* ==================== 渲染循环 ==================== */

    private fun startRender(surface: Surface) {
        val sv = overlay ?: return
        val w = sv.width.takeIf { it > 0 } ?: resources.displayMetrics.widthPixels
        val h = sv.height.takeIf { it > 0 } ?: resources.displayMetrics.heightPixels
        render = RenderThread(surface, w, h).also {
            it.isDaemon = true
            it.start()
        }
    }

    private inner class RenderThread(
        private val surface: Surface,
        private var w: Int,
        private var h: Int,
    ) : Thread() {

        @Volatile private var run = true
        private var probeTick = 0

        /** 主题自动轮询的计时器（秒）。见 [run] 里的轮询段。 */
        private var themeClock = 0.0

        /** 上次夹参数的时刻（nanoTime）。每 2 秒兜底夹一次，见 [run]。 */
        private var lastClamp = 0L

        fun setSize(nw: Int, nh: Int) {
            w = max(1, nw)
            h = max(1, nh)
        }

        fun stopLoop() {
            run = false
        }

        override fun run() {
            val gl = GlRenderer()
            if (!gl.init(surface, w, h)) {
                Log.e(TAG, "GL 初始化失败: ${gl.lastError}")
                return
            }
            val p = FrameParams()
            val pcm = FloatArray(LP_FFT_SIZE * 2 * 4)
            var last = System.nanoTime()
            var t = 0.0

            while (run) {
                val now = System.nanoTime()
                var dt = (now - last) / 1e9
                last = now
                /* 卡顿/息屏回来时 dt 会很大，钳一下免得分析层一次补跑几百轮 */
                if (dt > 0.25) dt = 0.25
                t += dt

                val s = store.settings

                /* ★★★ 「参数实时生效」的落地点就在这一行。
                   面板改的是**同一个** SettingsStore（见 [SettingsHolder]），
                   所以这里每帧同步一次，**下一帧画面就是新参数** ——
                   不需要停掉覆盖层再重开。
                   syncAnalyzer 内部自己判断「要不要重建频段/滚动直方图」，
                   常态下（什么都没改）几乎零开销。 */
                syncAnalyzer()

                /* 主题自动轮询 —— 逐字照抄 Windows 版 `main.cpp` 渲染循环里的那段：
                   ```
                   if (g_set.themeAuto) {
                     g_themeClock += dt;
                     if (g_themeClock >= g_set.themeCycleSec) {
                       g_themeClock = 0; g_set.themeIndex = (g_set.themeIndex + 1) % LP_THEME_COUNT;
                     }
                   }
                   ```
                   ⚠ Windows 版特意加了「面板打开时不轮询」，怕把用户在下拉里的
                     可视选择覆盖掉。安卓这边**不需要**这个限制：设置页是一个
                     不透明的 Activity，把整个屏幕连光柱一起盖住了，用户看不到画面；
                     而 `themeAuto` 一旦为真，下拉里显示的就是「自动轮询」这一项，
                     轮询不会让任何控件显示与实际不符。 */
                if (s.themeAuto) {
                    themeClock += dt
                    if (themeClock >= s.themeCycleSec) {
                        themeClock = 0.0
                        s.themeIndex = (s.themeIndex + 1) % LP_THEMES.size
                    }
                } else {
                    themeClock = 0.0
                }

                /* 兜底钳制：ini 是用户能直接改的文本（adb push 也会改它），
                   越界值（尤其 `hiPct <= loPct` 会让归一化除零）必须在跑之前夹回来。
                   ⚠ 每 2 秒一次就够 —— `clamp()` 里有 20 多个分支，每帧跑纯浪费；
                     而且正常使用下它永远不改动任何值（只在异常时生效）。 */
                if (now - lastClamp > 2_000_000_000L) {
                    lastClamp = now
                    s.clamp()
                }

                // 音频推进
                val n = audio.window.snapshot(pcm, pcm.size)
                if (n > 0) analyzer.tick(dt, pcm, n) else analyzer.tick(dt, pcm, 0)

                // 打包 uniform
                val th = themeAt(s.themeIndex)
                p.time = t.toFloat()
                p.styleMode = s.styleMode.toFloat()
                p.beamInset = s.beamInset
                p.audioIntensity = s.audioIntensity
                p.responseRange = s.responseRange
                p.idleWave = analyzer.idleWave
                val b = analyzer.bands
                p.subBass = b.subBass
                p.bass = b.bass
                p.lowMid = b.lowMid
                p.mid = b.mid
                p.highMid = b.highMid
                p.energy = b.energy
                p.presence = b.presence
                p.brilliance = b.brilliance
                p.air = b.air
                p.brightness = b.brightness
                p.cool = th.cool
                p.warm = th.warm
                p.peak = th.peak
                p.glow = th.glow
                p.peakEnabled = if (s.peakEnabled) 1f else 0f
                p.peakIntensity = s.peakIntensity
                p.alphaK = s.alphaK
                p.debugMode = s.debugMode.toFloat()

                gl.setViewport(w, h)
                gl.draw(p)

                /* ★ 诊断探针：每秒回读一次 framebuffer。
                   真机上「光柱不显示」时，这一行是唯一能区分
                   「着色器算出来是黑的」和「算对了但被合成器丢了」的手段。
                   读数非零 + 屏幕全黑 ⇒ 一定是合成侧问题。 */
                if (++probeTick >= 60) {
                    probeTick = 0
                    val pr = gl.probe()
                    val low = b.subBass * 1.20 + b.bass * 0.85 + b.energy * 0.25
                    val drive = (low * p.audioIntensity * p.responseRange).coerceIn(0.0, 1.70)
                    /* ★★ 这一行必须跟着**着色器**的 debugMode 覆盖走。
                       ⚠ 踩过：debugMode=12 时着色器把驱动量钉死在 0.90，而这里还打印
                         真实音频算出来的 0.24 —— 日志自相矛盾，排查的人会得出
                         「固定驱动没生效」去查一个根本不存在的问题。
                         （本轮验证 beamInset 时就被它绊了一下：探针说 drive=0.066，
                           而画面几何明明对应 0.90。）
                       固定档位见 Shaders.kt 里 `uDebugMode > 11.5` 那几行。 */
                    val fixed = when (s.debugMode) {
                        12 -> 0.90f
                        13 -> 0.20f
                        14 -> 0.10f
                        else -> null
                    }
                    val shown = fixed ?: drive
                    Log.i(
                        TAG,
                        String.format(
                            "probe max=(%d,%d,%d,%d)@(%d,%d GL) 左轴=(%d,%d,%d) 右轴A=%d | " +
                                "glow=%.2f alphaK=%.2f idle=%.3f drive=%.3f%s " +
                                "sub=%.3f bass=%.3f eng=%.3f | vp=%dx%d",
                            pr[0], pr[1], pr[2], pr[3], pr[8], pr[9],
                            pr[4], pr[5], pr[6], pr[7],
                            p.glow, p.alphaK, p.idleWave, shown,
                            if (fixed != null) "(debug 固定，非真实音频)" else "",
                            b.subBass, b.bass, b.energy, w, h
                        )
                    )
                    /* ★ debugMode = 9：把 framebuffer 原样导出成 PNG，用于「看到」渲染结果。
                       真机上「probe 有读数但屏幕全黑」时，这张图是唯一能分清
                       「画错位置」和「没上屏」的证据。 */
                    if (s.debugMode == 9 || s.debugMode == 12) {
                        val png = gl.dumpFramebuffer()
                        if (png != null) {
                            try {
                                File(filesDir, "fb_dump.png").writeBytes(png)
                                Log.i(TAG, "framebuffer 已导出: ${png.size} bytes")
                            } catch (e: Exception) {
                                Log.e(TAG, "导出 framebuffer 失败", e)
                            }
                        }
                    }
                }

                if (!gl.swap()) {
                    Log.w(TAG, "eglSwapBuffers 失败，结束渲染线程")
                    break
                }

                // 帧率限制
                val target = if (s.fpsLimit > 0) s.fpsLimit else 60
                val frameNs = 1_000_000_000L / target
                val spent = System.nanoTime() - now
                val sleepNs = frameNs - spent
                if (sleepNs > 1_000_000L) {
                    try {
                        sleep(sleepNs / 1_000_000L, (sleepNs % 1_000_000L).toInt())
                    } catch (_: InterruptedException) {
                        break
                    }
                }
            }

            gl.release()
        }
    }
}

/**
 * MediaProjection 授权结果的交接处（Activity → Service）。
 *
 * ★★★ 这里传的是**原始授权结果** `(resultCode, data)`，**不是**已经拿到的
 *     `MediaProjection` 对象。这个区别是必须的，踩过一次真机闪退：
 *
 *     Android 10（API 29）起，`MediaProjectionManager.getMediaProjection()` 会检查
 *     调用方**此刻**有没有一个 `foregroundServiceType=mediaProjection` 的前台服务
 *     在运行。没有就直接抛：
 *         SecurityException: Media projections require a foreground service of type
 *         ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
 *     而且它是抛在 Activity 的 `onActivityResult` 里 —— 表现是
 *     **用户一选「内部音频」、点了授权，程序立刻闪退**，连 toast 都来不及弹。
 *
 *     所以顺序只能是，一步都不能换：
 *       Activity 拿到 (resultCode, data)
 *         → 交给服务（本对象）
 *         → 服务先 startForeground(type = mediaProjection)
 *         → **然后**才 getMediaProjection()
 *
 *     「先在 Activity 里拿到、再传给服务」看着更直接，但一定是崩的。
 *
 * ⚠ 结果是一次性的：`takeResult()` 取走就清空，避免服务被重复 start 时
 *   反复拿同一个授权（第二次拿会失败，且日志会被刷屏）。
 */
object ProjectionHolder {
    @Volatile private var code: Int = 0
    @Volatile private var data: Intent? = null
    @Volatile private var p: MediaProjection? = null

    fun setResult(resultCode: Int, intent: Intent?) {
        code = resultCode
        data = intent
    }

    fun hasResult(): Boolean = data != null

    /** 取走授权结果（取完即清）。服务在前台起来之后调。 */
    fun takeResult(): Pair<Int, Intent>? {
        val d = data ?: return null
        val c = code
        data = null
        code = 0
        return c to d
    }

    fun set(v: MediaProjection?) {
        p = v
    }

    fun get(): MediaProjection? = p

    /** 服务销毁时调用：不 stop 的话系统状态栏会一直留着「正在录制/投放」的提示。 */
    fun release() {
        try {
            p?.stop()
        } catch (_: Exception) {
        }
        p = null
    }
}
