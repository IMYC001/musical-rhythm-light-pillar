package com.lightpillar

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/* ---- ② 的参数（对应原作品 browser-bridge 里的同名常量）---- */
const val LP_CH_BINS = 64          // 每声道 64 段
const val LP_OUT_BINS = 128        // 64 左 + 64 右
const val LP_FFT_SIZE = 2048
const val LP_MAX_HZ = 16000.0
/** 等比（log）频率轴的下限，对应 bridge 的 `BAND_LO_HZ = 30`。 */
const val LP_BAND_LO_HZ = 30.0
const val LP_DB_MIN = -100.0
const val LP_DB_MAX = -20.0
const val LP_LV_MIN0 = -53.5
const val LP_SPECTRAL_TILT = 5.5
const val LP_TILT_REF_HZ = 125.0

/* ---- 段电平自适应（perband）---- */
const val LP_RP_BINS = 64
const val LP_RP_DBMIN = -110.0
const val LP_RP_DBMAX = -10.0

private const val LP_IDLE_ENERGY_THRESHOLD = 0.02

/** 已平滑的频段包络，直接喂给着色器 uniform。 */
class LpBands {
    var subBass = 0f
    var bass = 0f
    var lowMid = 0f
    var mid = 0f
    var highMid = 0f
    var energy = 0f
    var presence = 0f
    var brilliance = 0f
    var air = 0f
    var brightness = 0f
}

/**
 * 音频分析链 —— 逐字移植自 Windows 版 `src/audio.cpp`。
 *
 * 四段链路：
 *   ① 采集（由 [AudioEngine] 填 pending 缓冲，立体声 float）
 *   ② 2048 点 FFT（Blackman 窗）→ 每声道 64 段电平 0~1   ← 等价 Web Audio AnalyserNode
 *   ③ 128 值（64 左 + 64 右）→ 4 倍上采样成 512 桶        ← 等价 setWallpaperAudioData
 *   ④ 512 桶 → 10 个频段包络 + 平滑                        ← 等价作品 bundle 里的 getAudioData
 *
 * ⚠ ②③④ 三段的常数一个都不能改。作品是在 WE 的 128 值格式下调的参，
 *   bridge 的作者又是照着 WE 的物理含义反推的 64 段划分（0–16kHz 等宽）。
 *   任何一处「等价改写」都会让 drive 的摆幅对不上，表现是光柱要么不动、要么顶满。
 *
 * ★★★ 三层频率，不能合并（原作品就是这么分的）：
 *     数据层 30Hz（FFT→128 值） / 分析层 anaHz=250Hz（→16 个平滑量） / 渲染层 fpsLimit
 *   Windows 版把这三个错法全踩过一遍，见 [tick] 里的长注释。
 */
class Analyzer {

    /* ---------------- 可调参数 ---------------- */
    var gainDb = 0.0
    var widthDb = 46.0
    var balance = 0.85
    var mapMode = 0          // 0 = perband（每段自适应）1 = window（全频段共用窗口）
    var bandMode = 0         // 0 = linear（0–16kHz 等宽）1 = log（30–16kHz 等比）
    var level = 0.15
    var contrast = 1.8
    var loPct = 0.20
    var hiPct = 0.99
    var minSpan = 4.0
    var rpWinSec = 10.0
    var intensity = 1.0
    var responseRange = 1.0
    var anaHz = 250.0
    var idleEnabled = true
    var idleDebounce = 1.0
    var idleFade = 1.0

    /* ---------------- 输出 ---------------- */
    val bands = LpBands()
    var idleWave = 0f
        private set
    var meterLevel = 0f
        private set

    /** 128 个频谱值（0~1）。自检/对照要用。 */
    val spectrum = DoubleArray(LP_OUT_BINS)

    /* ---------------- ② 的中间量 ---------------- */
    private val bandK0 = IntArray(LP_CH_BINS)
    private val bandK1 = IntArray(LP_CH_BINS)
    private val bandTilt = DoubleArray(LP_CH_BINS)
    private val fftRe = DoubleArray(LP_FFT_SIZE)
    private val fftIm = DoubleArray(LP_FFT_SIZE)
    private val win = DoubleArray(LP_FFT_SIZE)
    private val rawDb = DoubleArray(LP_OUT_BINS)
    private var lvMin = LP_LV_MIN0
    private var lvMax = LP_LV_MIN0 + 46.0

    /* 滚动直方图 */
    private var rpRing = ShortArray(0)
    private var rpHist = IntArray(0)
    private var rpWin = 300
    private var rpPos = 0
    private var rpFrame = 0

    /* ---------------- ③④ 的中间量 ---------------- */
    private val bucket = DoubleArray(512)
    private val prevBucket = DoubleArray(512)
    private val smooth = DoubleArray(16)
    private var prevBrightness = 0.0
    private var lastActiveTime = 0.0
    private var lastIdleTime = 0.0
    private var idleStartTime = 0.0
    private var curIdle = 0.0
    private var nowMs = 0.0
    private var specAccum = 0.0
    private var anaAccum = 0.0
    private var audioReceived = false

    /* ⚠ 这两个必须**可写**：AudioEngine 打开设备之后才知道真实的采样率和声道数，
       由 OverlayService 在同步参数时写进来。写成 `private set` 编译不过。 */
    var sampleRate = 48000.0
    var channels = 2

    init {
        /* Blackman 窗，和 Web Audio 的 AnalyserNode 用的一致。
           ⚠ 窗函数直接决定「段电平 → dB」的绝对刻度，但 perband 映射是
             **自归一化**的（用该段自己的滚动分位当上下限），所以刻度整体平移
             不影响结果 —— 只有「频谱形状」和「段内动态范围」是敏感的。 */
        for (n in 0 until LP_FFT_SIZE) {
            val t = n.toDouble() / LP_FFT_SIZE.toDouble()
            win[n] = 0.42 - 0.5 * cos(2.0 * PI * t) + 0.08 * cos(4.0 * PI * t)
        }
        for (i in 0 until LP_OUT_BINS) {
            spectrum[i] = 0.0
            rawDb[i] = -200.0
            bandK0[i % LP_CH_BINS] = 0
            bandK1[i % LP_CH_BINS] = 1
        }
        resetTrackers()
    }

    /* ==================== 频段划分 ==================== */

    /**
     * 第 i 段的下边界频率（Hz）。逐字对应 bridge 的
     * ```
     * function bandEdge(i) {
     *   if (BAND_MODE === 'log') return BAND_LO_HZ * Math.pow(MAX_HZ / BAND_LO_HZ, i / CH_BINS);
     *   return MAX_HZ * i / CH_BINS;
     * }
     * ```
     * ⚠ 别在这里另写一份「看起来一样」的公式：[buildBands] 和 [rebuildTilt]
     *   必须取到**同一个**边界，否则切到等比之后频段划分变了、倾斜补偿量却没变。
     */
    fun bandEdgeHz(i: Double): Double {
        if (bandMode == 1) {   // log：30–16k 等比
            return LP_BAND_LO_HZ * (LP_MAX_HZ / LP_BAND_LO_HZ).pow(i / LP_CH_BINS.toDouble())
        }
        return LP_MAX_HZ * i / LP_CH_BINS.toDouble()   // linear：0–16k 等宽
    }

    fun rebuildTilt() {
        for (i in 0 until LP_CH_BINS) {
            /* ★ 参考频率取自**当前**频段划分（和原作品一致）。
               Windows 版 v6 之前这里写死了 linear 的公式 —— 等比模式下每段的
               中心频率完全不同、补偿量却按等宽算，是第二个偏差。 */
            val fc = max(0.5 * (bandEdgeHz(i.toDouble()) + bandEdgeHz(i + 1.0)), LP_TILT_REF_HZ)
            bandTilt[i] = balance * LP_SPECTRAL_TILT * (kotlin.math.ln(fc / LP_TILT_REF_HZ) / kotlin.math.ln(2.0))
        }
    }

    fun buildBands() {
        val nyq = sampleRate / 2.0
        val fftBins = LP_FFT_SIZE / 2          // AnalyserNode.frequencyBinCount
        val binHz = nyq / fftBins.toDouble()

        for (j in 0 until LP_CH_BINS) {
            val e0 = bandEdgeHz(j.toDouble())
            val e1 = bandEdgeHz(j + 1.0)
            var k0 = floor(e0 / binHz).toInt()
            var k1 = floor(e1 / binHz).toInt()
            if (k1 <= k0) k1 = k0 + 1
            if (k0 > fftBins - 1) k0 = fftBins - 1
            if (k1 > fftBins) k1 = fftBins
            bandK0[j] = k0
            bandK1[j] = k1
        }
        rebuildTilt()
        lvMin = LP_LV_MIN0 - gainDb
        lvMax = lvMin + widthDb
    }

    /** 只重算 dB 窗口与倾斜表，**不动**滚动直方图。
     *  拖「增益 / 对比度 / 总电平」这类纯数值滑杆时用这个 ——
     *  每次都清空直方图会让面板看着像坏的（要重新攒满 10 秒才准）。 */
    fun rebuildTiltOnly() {
        lvMin = LP_LV_MIN0 - gainDb
        lvMax = lvMin + widthDb
        rebuildTilt()
    }

    /**
     * 自检用只读口：滚动直方图的帧数（= `rpWinSec × 30Hz`，见 [resetTrackers]）。
     *
     * 为什么值得暴露：这个换算里藏着一个**平台无关的硬规则** ——
     * 直方图挂在**频谱层**（固定 30Hz），不是渲染帧率。写错的话表现是
     * 「跟踪窗口改大改小，观感一样」或「改完要等很久才稳定」，
     * 而这两种现象在屏幕上都没法和「正常」区分。有个读数才能断言。
     */
    val trackWindowFrames: Int get() = rpWin

    fun resetTrackers() {
        /* 窗口长度可以调（界面上的「跟踪窗口」，秒）。
           ★ 固定按 **30Hz** 换算成帧数 —— 直方图挂在**频谱**那一层，
             而频谱固定跑 30Hz（= 原作品 browser-bridge.js 的 PUSH_HZ = 30）。
             所以「渲染帧率」怎么改都不会动到这里，也就不需要重新攒 10 秒。 */
        val hz = 30
        var want = Math.round(rpWinSec * hz).toInt()
        if (want < hz) want = hz
        if (want > hz * 30) want = hz * 30
        rpWin = want

        val need = LP_OUT_BINS * rpWin
        if (rpRing.size != need) rpRing = ShortArray(need) else rpRing.fill(0)
        val needH = LP_OUT_BINS * LP_RP_BINS
        if (rpHist.size != needH) rpHist = IntArray(needH) else rpHist.fill(0)
        rpPos = 0
        rpFrame = 0
    }

    fun paramsChanged() {
        buildBands()
        resetTrackers()
    }

    /** 改了 anaHz 之后调一下，把分析累加器清零 ——
     *  否则会拿旧周期算出来的余量去按新周期补跑一批分析，
     *  表现是「改分析频率的那一瞬间包络突跳一下」。
     *  ⚠ 只清累加器，**不动**滚动直方图。 */
    fun resetAnalysisClock() {
        anaAccum = 0.0
    }

    /** 频段划分表的指纹（只取决于 bandK0/bandK1，与音频数据无关）。
     *  自检用它证明「频率轴」这一项真的生效了 —— 这一项曾经切了完全没反应：
     *  控件值会变，但频段划分表只在开音频时建过一次。 */
    fun bandSignature(): Long {
        var h = -3750763034362895579L   // 1469598103934665603 as signed
        for (i in 0 until LP_CH_BINS) {
            h = (h xor bandK0[i].toLong()) * 1099511628211L
            h = (h xor bandK1[i].toLong()) * 1099511628211L
        }
        return h
    }

    /* ==================== ② 64 段电平 ==================== */

    /** 迭代式基-2 Cooley–Tukey。N=2048、每秒 30 次，开销可以忽略。 */
    private fun fftRadix2(re: DoubleArray, im: DoubleArray, n: Int) {
        var j = 0
        for (i in 1 until n) {
            var bit = n shr 1
            while (j and bit != 0) {
                j = j xor bit
                bit = bit shr 1
            }
            j = j xor bit
            if (i < j) {
                val tr = re[i]; re[i] = re[j]; re[j] = tr
                val ti = im[i]; im[i] = im[j]; im[j] = ti
            }
        }
        var len = 2
        while (len <= n) {
            val ang = -2.0 * PI / len.toDouble()
            val wr = cos(ang)
            val wi = sin(ang)
            val half = len shr 1
            var i = 0
            while (i < n) {
                var cr = 1.0
                var ci = 0.0
                for (k in 0 until half) {
                    val a = i + k
                    val b = a + half
                    val ur = re[a]
                    val ui = im[a]
                    val vr = re[b] * cr - im[b] * ci
                    val vi = re[b] * ci + im[b] * cr
                    re[a] = ur + vr
                    im[a] = ui + vi
                    re[b] = ur - vr
                    im[b] = ui - vi
                    val ncr = cr * wr - ci * wi
                    ci = cr * wi + ci * wr
                    cr = ncr
                }
                i += len
            }
            len = len shl 1
        }
    }

    /**
     * ② 从交错 PCM 取最近 2048 帧 → FFT → 每声道 64 段电平。
     *
     * @param pcm  交错 float 采样（LRLRLR…），[pcmOff] 起有效
     * @param count 有效采样数
     */
    fun computeSpectrum128(pcm: FloatArray, count: Int) {
        val ch = channels.coerceAtLeast(1)
        val frames = count / ch
        val left = DoubleArray(LP_FFT_SIZE)
        val right = DoubleArray(LP_FFT_SIZE)

        /* ⚠⚠ 这里是整条链最容易写错的地方：**读的是「最近 2048 帧」这个滑动窗口**，
           不是「本帧新增的那点数据」。原作品的 AnalyserNode 是一个随音频采样率
           连续滑动的 2048 点窗口，getByteFrequencyData 只是读、不消费。
           如果这里只拿「本帧新增」（48kHz 下每帧约 800 个采样），永远凑不满 2048
           —— FFT 输入全 0、频谱恒为 -100dB，光柱看着还在动（空闲波浪在撑），
           但完全不响应声音。 */
        if (frames >= LP_FFT_SIZE) {
            val off = (frames - LP_FFT_SIZE) * ch
            for (i in 0 until LP_FFT_SIZE) {
                val p = off + i * ch
                left[i] = pcm[p].toDouble()
                right[i] = if (ch > 1) pcm[p + 1].toDouble() else pcm[p].toDouble()
            }
        } else {
            return   // 还没攒够一个窗口，本帧不更新（保持上一次的频谱）
        }

        val w = lvMax - lvMin
        val rpBinW = (LP_RP_DBMAX - LP_RP_DBMIN) / LP_RP_BINS

        for (c in 0 until 2) {
            val src = if (c == 0) left else right
            for (i in 0 until LP_FFT_SIZE) {
                fftRe[i] = src[i] * win[i]
                fftIm[i] = 0.0
            }
            fftRadix2(fftRe, fftIm, LP_FFT_SIZE)

            val offset = c * LP_CH_BINS
            for (i in 0 until LP_CH_BINS) {
                val k0 = bandK0[i]
                val k1 = bandK1[i]
                val cnt = k1 - k0
                var power = 0.0
                for (k in k0 until k1) {
                    /* Chromium 的 AnalyserNode 把 FFT 结果除以 fftSize 后再取 dB，
                       这里跟着除，保证 dB 的绝对刻度一致。 */
                    val mag = sqrt(fftRe[k] * fftRe[k] + fftIm[k] * fftIm[k]) / LP_FFT_SIZE.toDouble()
                    val db = 20.0 * log10(mag + 1e-30)
                    /* 复刻 getByteFrequencyData 的量化 + 截断（DB_MIN..DB_MAX 之外
                       会被夹住，这一步不能省 —— 作品的段电平是在夹过的值上继续算的）*/
                    var norm = (db - LP_DB_MIN) / (LP_DB_MAX - LP_DB_MIN)
                    norm = norm.coerceIn(0.0, 1.0)
                    val byte = Math.round(norm * 255.0).toInt()
                    val dbq = LP_DB_MIN + (byte / 255.0) * (LP_DB_MAX - LP_DB_MIN)
                    power += 10.0.pow(dbq / 10.0)
                }
                if (cnt > 0) power /= cnt.toDouble()
                val bandDb = 10.0 * log10(power + 1e-12)
                val slot = offset + i
                rawDb[slot] = bandDb

                if (mapMode == 0) {
                    /* perband：把本帧电平丢进该段的滚动直方图，再从直方图读
                       p_lo / p_hi，得到「该段自己的动态范围」。
                       ★ 这里不需要任何抽稀：整条 computeSpectrum128 已固定跑 30Hz
                         （原作品 PUSH_HZ = 30），喂进直方图的样本天然就是 30Hz，
                         窗口 300 帧 = 10 秒。 */
                    val base = slot * LP_RP_BINS
                    var bi = floor((bandDb - LP_RP_DBMIN) / rpBinW).toInt()
                    if (bi < 0) bi = 0 else if (bi > LP_RP_BINS - 1) bi = LP_RP_BINS - 1
                    val ridx = slot * rpWin + rpPos
                    if (rpFrame >= rpWin && rpHist[base + rpRing[ridx]] > 0)
                        rpHist[base + rpRing[ridx]]--
                    rpRing[ridx] = bi.toShort()
                    if (rpHist[base + bi] < 65535) rpHist[base + bi]++

                    val total = if (rpFrame >= rpWin) rpWin.toDouble() else (rpFrame + 1).toDouble()
                    val tlo = total * loPct
                    val thi = total * hiPct
                    var acc = 0.0
                    var lo = LP_RP_DBMIN
                    var hi = LP_RP_DBMAX
                    var gotLo = false
                    for (k in 0 until LP_RP_BINS) {
                        acc += rpHist[base + k]
                        if (!gotLo && acc >= tlo) {
                            gotLo = true
                            lo = LP_RP_DBMIN + (k + 0.5) * rpBinW
                        }
                        if (acc >= thi) {
                            hi = LP_RP_DBMIN + (k + 0.5) * rpBinW
                            break
                        }
                    }
                    var span = hi - lo
                    if (span < minSpan) span = minSpan
                    var lin = (bandDb - lo) / span
                    lin = lin.coerceIn(0.0, 1.0)
                    spectrum[slot] = lin.pow(contrast) * level
                } else {
                    val lv = (bandDb + bandTilt[i] - lvMin) / w
                    spectrum[slot] = lv.coerceIn(0.0, 1.0)
                }
            }
        }

        /* ★ 这里必须是 rpWin 而不是 300！
           「跟踪窗口」可以调到别的秒数 → rpWin 可能不是 300。
           用 300 取模的话环形缓冲只会在前 300 格上循环、后面的格子永远写不到，
           而直方图里的计数还在被加/减 —— 分位会一路漂到错的地方。
           这个 bug 在默认值（10 秒 / 30Hz → 恰好 300）下完全看不出来。 */
        rpPos = (rpPos + 1) % rpWin
        rpFrame++
    }

    /* ==================== ③④ 分析链 ==================== */

    fun analyze512() {
        /* ③ 128 → 512：最近邻上采样，每个输入值占 4 个输出桶
              （对应作品里的 Math.floor(n * t / 512)）*/
        for (n in 0 until 512) {
            val idx = floor(n.toDouble() * LP_OUT_BINS / 512.0).toInt()
            bucket[n] = spectrum[if (idx < LP_OUT_BINS) idx else LP_OUT_BINS - 1]
        }

        /* ④ 作品 bundle 里的 getAudioData，逐项照抄 */
        val n = 512
        var r = 0.0; var o = 0.0; var cFlux = 0.0
        var h = 0.0; var d = 0.0; var m = 0.0; var v = 0.0
        var y = 0.0; var x = 0.0; var b = 0.0; var mm = 0.0

        for (i in 0 until n) {
            val de = bucket[i]
            r += de
            o += i.toDouble() * de
            cFlux += abs(de - prevBucket[i])
            when {
                i <= 4 -> h += de
                i <= 12 -> d += de
                i <= 24 -> m += de
                i <= 45 -> v += de
                i <= 81 -> y += de
                i <= 120 -> x += de
                i <= 180 -> b += de
                i <= 255 -> mm += de
            }
            prevBucket[i] = de
        }

        val rr = r / n
        /* ⚠ 分母比桶数大 1：作品原文是 h/5、d/9、m/13、v/21、y/37、x/40、b/61、M/76，
           桶数分别是 5/8/12/21/36/39/60/75。也就是说**只有 h 是整除的**，
           其余一律多除了 1。这是原作者调过的刻度，照抄，不要「修正」。 */
        val lSub = h / 5.0
        val iBass = d / 9.0
        val zLowMid = m / 13.0
        val uMid = v / 21.0
        val fHighMid = y / 37.0
        val pPresence = x / 40.0
        val bBrill = b / 61.0
        val wAir = mm / 76.0

        val xBass = (h + d + m) / 27.0
        val jMid = (v + y) / 58.0
        val seTreble = (x + b + mm) / 177.0
        val ceWarmth = if (r > 0) (h + d + m + v) / r else 0.0
        val qBright = if (r > 0) (x + b + mm) / r else 0.0
        val ySharp = max(0.0, qBright - prevBrightness) * 10.0
        prevBrightness = qBright
        val kSmooth = max(0.0, 1.0 - cFlux / n * 2.0)

        val le = rr * 1.5
        var fe = 0
        if (lSub > le) fe++
        if (iBass > le) fe++
        if (zLowMid > le) fe++
        if (uMid > le) fe++
        if (fHighMid > le) fe++
        if (pPresence > le) fe++
        if (bBrill > le) fe++
        if (wAir > le) fe++
        val reDensity = fe / 8.0
        val qCentroid = if (r > 0) o / r else 0.0

        /* ★★★ 一阶平滑系数：**必须用原作品的原始值，一次都不能换算**。
         *
         *  原作品 bundle 里（getAudioData）：
         *      const ee = r > 0 ? .15 : .08;
         *      this.smoothedData.subBass += (L - this.smoothedData.subBass) * ee;
         *      …（16 个量都是这一个系数）
         *  它是**每次调用作用一次**的固定系数，而且**没有**按 dt 缩放 ——
         *  同一个函数里的节拍检测器倒是按 dt 缩放了（1 - 0.6^(dt*60)），
         *  说明作者是有意这么写的。而它跑在 requestAnimationFrame 里，
         *  频率由 FPS_LIMIT 决定（browser-bridge.js 里默认 60）。
         *
         *  ⚠ Windows 版 v5 在这里犯过一个错：把 ee 当成「按 30Hz 推送、每次作用
         *    一次」，于是按相同时间常数换算成 ee_eff = 1-(1-0.15)^(30/pushHz)，
         *    在 60fps 下得到 0.0780。结果时间常数变成 0.214s，是原作品的 1.93 倍
         *    （原作品 0.111s）→ **包络峰值只剩 67%**，
         *    表现就是用户报的「响应幅度没有 Electron 大、部分节拍不响应」。
         *
         *  正确做法：分析层跟着**真实时间**按 anaHz 跑，系数原样用 0.15 / 0.08。 */
        val ee = if (r > 0) 0.15 else 0.08
        smooth[0] += (xBass - smooth[0]) * ee      // bass
        smooth[1] += (jMid - smooth[1]) * ee       // mid
        smooth[2] += (seTreble - smooth[2]) * ee   // treble
        smooth[3] += (rr - smooth[3]) * ee         // energy
        smooth[4] += (lSub - smooth[4]) * ee       // subBass
        smooth[5] += (zLowMid - smooth[5]) * ee    // lowMid
        smooth[6] += (fHighMid - smooth[6]) * ee   // highMid
        smooth[7] += (pPresence - smooth[7]) * ee  // presence
        smooth[8] += (bBrill - smooth[8]) * ee     // brilliance
        smooth[9] += (wAir - smooth[9]) * ee       // air
        smooth[10] += (ceWarmth - smooth[10]) * ee // warmth
        smooth[11] += (qBright - smooth[11]) * ee  // brightness
        smooth[12] += (ySharp - smooth[12]) * ee   // sharpness
        smooth[13] += (kSmooth - smooth[13]) * ee  // smoothness
        smooth[14] += (reDensity - smooth[14]) * ee // density
        smooth[15] += (qCentroid - smooth[15]) * ee // spectralCentroid

        bands.bass = smooth[0].toFloat()
        bands.mid = smooth[1].toFloat()
        bands.subBass = smooth[4].toFloat()
        bands.lowMid = smooth[5].toFloat()
        bands.highMid = smooth[6].toFloat()
        bands.energy = smooth[3].toFloat()
        bands.presence = smooth[7].toFloat()
        bands.brilliance = smooth[8].toFloat()
        bands.air = smooth[9].toFloat()
        bands.brightness = smooth[11].toFloat()

        /* 活跃 / 空闲判定，给空闲波浪用 */
        if (rr > LP_IDLE_ENERGY_THRESHOLD) lastActiveTime = nowMs else lastIdleTime = nowMs
        if (!audioReceived) {
            audioReceived = true
            lastActiveTime = nowMs
            lastIdleTime = 0.0
        }

        /* 电平表：和 bridge 的 updateMeter 同一套算式（sum/128*5.5，夹到 1）*/
        var sum = 0.0
        for (i in 0 until LP_OUT_BINS) sum += spectrum[i]
        meterLevel = min(1.0, (sum / LP_OUT_BINS) * 5.5).toFloat()
    }

    /**
     * 渲染循环每帧调一次：推进三个频率层、更新 bands / idleWave。
     *
     * ★★★ 这里有**三个不同的频率**，原作品就是这么分的，不能合并。
     *
     *  ① **数据层**：FFT → 每声道 64 段 → 128 个频谱值。固定 **30Hz**。
     *     出处：browser-bridge.js 的 `var PUSH_HZ = 30;`
     *     （注释写着「壁纸引擎约 30 次/秒」）。滚动直方图挂在这一层。
     *
     *  ② **分析层**：128 → 512 桶上采样 → 8 个频段和 → 谱通量 / 重心 →
     *     16 个量的一阶平滑。固定 **anaHz（默认 250）**。
     *     出处：bundle 里 `setInterval(ht, 1e3/1e3)`，请求周期 1ms，
     *     Chromium 把嵌套定时器钳到 4ms → 实测 **249.6 次/秒**。
     *
     *  ③ **渲染层**：`general.fps`（默认 60）驱动着色器 uniform。
     *
     *  ⚠ 三个错法，Windows 版全踩过：
     *    · 把 ①② 都钉在 30Hz（v1~v4）→ 「帧率调节没用，每个选项观感都一样」；
     *    · 把 ①② 都挂在渲染帧率上、再把 ee 按 30Hz 做时间常数换算（v5）→
     *      分析层 60Hz 且 τ×1.93 → 幅度只剩 67% → 「幅度没有 Electron 大」；
     *    · 只修 ee 不修频率（v6 初版）→ 分析层仍是 60Hz，τ=111ms，
     *      而桌面版实测是 250Hz、τ=24.6ms → **仍然慢 4.5 倍**。
     *
     *  ★ 关键：分析层**不跟渲染帧率走**，跟的是**真实时间**。
     *    所以把 fpsLimit 调到 30 或 144 都**不该**改变包络形状 ——
     *    只改变画面把它画出来的细腻程度。
     *
     *  @param dt      距上一帧的秒数
     *  @param pcm     交错 PCM 缓冲
     *  @param count   有效采样数
     */
    fun tick(dt: Double, pcm: FloatArray, count: Int) {
        nowMs += dt * 1000.0

        val specDt = 1.0 / 30.0
        specAccum += dt
        var guard = 0
        while (specAccum >= specDt && guard++ < 4) {
            specAccum -= specDt
            computeSpectrum128(pcm, count)
        }
        if (specAccum > specDt * 4.0) specAccum = 0.0   // 卡顿后别追帧追爆

        /* 分析层：按 anaHz 跑（默认 250，= 桌面版实测的那个定时器频率）。
           渲染 60fps 时每帧约 4.2 次 —— 和桌面版一样：渲染一帧，
           分析已经在后台跑过好几轮了，画面读到的永远是「最近一次」的结果。
           ⚠ guard 要给够：渲染掉到 30fps 时每帧要补 ~8 次；
             卡顿后最多补 64 次，再多就说明是长时间挂起，直接放弃追赶。 */
        val anaDt = 1.0 / (if (anaHz > 1.0) anaHz else 1.0)
        anaAccum += dt
        guard = 0
        while (anaAccum >= anaDt && guard++ < 64) {
            anaAccum -= anaDt
            analyze512()
        }
        if (anaAccum > anaDt * 64.0) anaAccum = 0.0

        /* 空闲波浪：连续安静 debounce 秒后淡入到 1，有声音立刻淡出到 0。
           ⚠ 过渡是按**时间**积分的（1/fade * dt），不是按帧 ——
             否则高刷屏上会快好几倍。 */
        val target: Double
        if (audioReceived) {
            if (lastActiveTime > lastIdleTime) {
                target = 0.0
                idleStartTime = 0.0
            } else {
                if (idleStartTime == 0.0) idleStartTime = nowMs
                target = if ((nowMs - idleStartTime) / 1000.0 >= idleDebounce) 1.0 else 0.0
            }
        } else {
            target = 1.0
        }
        val step = (1.0 / max(0.01, idleFade)) * dt
        if (target > curIdle) curIdle = min(target, curIdle + step)
        else if (target < curIdle) curIdle = max(target, curIdle - step)
        idleWave = if (idleEnabled) curIdle.toFloat() else 0f
    }

    /* ---- 测试钩子：把 128 个频谱值直接塞进来、跑一次分析 ----
     *  用于「原生分析链 vs 原作品 JS 分析链」喂同一串输入、逐帧比 16 个平滑量。
     *  ⚠ 调用方必须自己按正确的**调用频率**喂：anaHz=250 就每 4ms 调一次。 */
    fun testFeedSpectrum(spec128: DoubleArray) {
        for (i in 0 until LP_OUT_BINS) spectrum[i] = spec128[i]
        analyze512()
    }

    /** 16 个平滑量的原始数组（顺序 = 原作品 smoothedData 的键顺序）。 */
    fun smoothed(): DoubleArray = smooth
}
