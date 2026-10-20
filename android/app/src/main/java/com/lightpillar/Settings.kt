package com.lightpillar

import java.io.File

enum class KType { INT, FLOAT, BOOL }

/**
 * 参数清单里的一项。
 *
 * C++ 版是 `io.vInt(L"styleMode", &g_set.styleMode, 0)` —— 传的是**指针**，
 * 所以同一个 visitAllSettings 能被三个方向的 IO 复用。
 * Kotlin 没有指针，于是把「取 / 存」包成一对 lambda：
 *   读方向用 get()，写方向用 set()。清单仍然只有一份。
 */
class KeyAcc(
    val name: String,
    val type: KType,
    val def: Double,
    val get: () -> Double,
    val set: (Double) -> Unit,
) {
    fun defStr(): String = when (type) {
        KType.INT -> def.toInt().toString()
        KType.FLOAT -> fmtG(def.toFloat())
        KType.BOOL -> if (def != 0.0) "1" else "0"
    }

    fun curStr(): String = when (type) {
        KType.INT -> get().toInt().toString()
        KType.FLOAT -> fmtG(get().toFloat())
        KType.BOOL -> if (get() != 0.0) "1" else "0"
    }
}

/**
 * 全部参数（27 键）。
 *
 * ★★★ 这份清单是**唯一**的，键名、类型、默认值必须和 Windows 版
 *     `src/main.cpp` 的 `visitAllSettings()` 逐字一致。
 *     它是两个平台做逐键对照的基准 —— 差一个键、差一个默认值，
 *     对照出来的差异就全是假的。
 */
class Settings {
    var styleMode = 0
    var beamInset = 0f
    var themeIndex = 0
    var themeAuto = false
    var themeCycleSec = 60f
    var audioIntensity = 1f
    var responseRange = 1f
    var peakEnabled = true
    var peakIntensity = 1f
    var idleEnabled = true
    var idleDebounce = 1f
    var idleFade = 1f
    var alphaK = 0.10f
    var fpsLimit = 60
    /** ★ 默认必须是 250，不能是 60。
     *  60 会让包络慢 4.5 倍 —— 正是用户报的「幅度没有桌面版大、部分节拍不响应」。
     *  出处：桌面版实测那个分析定时器跑 249.6 次/秒。 */
    var anaHz = 250
    var displayIndex = -1
    var gainDb = 0f
    var widthDb = 46f
    var balance = 0.85f
    var mapMode = 0
    var bandMode = 0
    var contrast = 1.8f
    var level = 0.15f
    var loPct = 0.20f
    var hiPct = 0.99f
    var rpWinSec = 10f
    var audioIdleMode = 0

    /* ---- 安卓特有的参数 ----
       ⚠ 刻意**不进**上面那份 keys 清单：那份 27 键清单是两个平台逐键对照的基准，
         加一个键就会让对照结果全是假差异。而且这些参数在 Windows 上根本不存在
         —— 那边只有 WASAPI loopback 一种来源，没得选。
         它们存在同一个 ini 的 `[android]` section 里。 */
    /** 0 = 麦克风（零摩擦默认）；1 = 内部音频（MediaProjection）。 */
    var audioSourceIndex = 0

    /**
     * ★ 诊断模式：0 = 正常；1/2/3 = 整屏纯红/绿/白不透明。
     *
     * 为什么不放进设置界面：它是**排障开关**，不是用户参数。用 adb 改 ini 的
     * `[android] debugMode` 就能测（见交付说明「诊断」一节），界面上不给入口，
     * 免得用户误触把整屏变红。
     *
     * 它的价值在于把「光柱不显示」这个笼统现象切成三种可区分的成因：
     *   读数(probe)全 0            → 着色器/参数问题
     *   读数非 0 + debug 1 能显示  → 图层在屏上，问题在 alpha/预乘合成
     *   读数非 0 + debug 1 不显示  → 图层压根没上屏，调 alpha 无用
     * 真机上这三种在截图里长得**一模一样**，没有这个开关就只能靠猜。
     */
    var debugMode = 0

    val keys: List<KeyAcc> = buildList {
        fun intKey(n: String, d: Int, g: () -> Int, s: (Int) -> Unit) =
            add(KeyAcc(n, KType.INT, d.toDouble(), { g().toDouble() }, { s(it.toInt()) }))

        fun floatKey(n: String, d: Float, g: () -> Float, s: (Float) -> Unit) =
            add(KeyAcc(n, KType.FLOAT, d.toDouble(), { g().toDouble() }, { s(it.toFloat()) }))

        fun boolKey(n: String, d: Boolean, g: () -> Boolean, s: (Boolean) -> Unit) =
            add(
                KeyAcc(
                    n, KType.BOOL, if (d) 1.0 else 0.0,
                    { if (g()) 1.0 else 0.0 },
                    { s(it != 0.0) },
                )
            )

        intKey("styleMode", 0, { styleMode }, { styleMode = it })
        floatKey("beamInset", 0f, { beamInset }, { beamInset = it })
        intKey("themeIndex", 0, { themeIndex }, { themeIndex = it })
        boolKey("themeAuto", false, { themeAuto }, { themeAuto = it })
        floatKey("themeCycleSec", 60f, { themeCycleSec }, { themeCycleSec = it })
        floatKey("audioIntensity", 1f, { audioIntensity }, { audioIntensity = it })
        floatKey("responseRange", 1f, { responseRange }, { responseRange = it })
        boolKey("peakEnabled", true, { peakEnabled }, { peakEnabled = it })
        floatKey("peakIntensity", 1f, { peakIntensity }, { peakIntensity = it })
        boolKey("idleEnabled", true, { idleEnabled }, { idleEnabled = it })
        floatKey("idleDebounce", 1f, { idleDebounce }, { idleDebounce = it })
        floatKey("idleFade", 1f, { idleFade }, { idleFade = it })
        floatKey("alphaK", 0.10f, { alphaK }, { alphaK = it })
        intKey("fpsLimit", 60, { fpsLimit }, { fpsLimit = it })
        intKey("anaHz", 250, { anaHz }, { anaHz = it })
        intKey("displayIndex", -1, { displayIndex }, { displayIndex = it })
        floatKey("gainDb", 0f, { gainDb }, { gainDb = it })
        floatKey("widthDb", 46f, { widthDb }, { widthDb = it })
        floatKey("balance", 0.85f, { balance }, { balance = it })
        intKey("mapMode", 0, { mapMode }, { mapMode = it })
        intKey("bandMode", 0, { bandMode }, { bandMode = it })
        floatKey("contrast", 1.8f, { contrast }, { contrast = it })
        floatKey("level", 0.15f, { level }, { level = it })
        floatKey("loPct", 0.20f, { loPct }, { loPct = it })
        floatKey("hiPct", 0.99f, { hiPct }, { hiPct = it })
        floatKey("rpWinSec", 10f, { rpWinSec }, { rpWinSec = it })
        intKey("audioIdleMode", 0, { audioIdleMode }, { audioIdleMode = it })
    }

    val byName: Map<String, KeyAcc> = keys.associateBy { it.name }

    fun visitAll(io: CfgIo) {
        for (k in keys) io.visit(k)
    }

    fun resetToDefaults() {
        for (k in keys) k.set(k.def)
    }

    /**
     * 把所有参数夹回合法范围。**载入 ini / 套用预设之后必须调一次。**
     *
     * ★★★ 为什么必须有（不是可选的健壮性装饰）：
     *   ini 和预设文件是**用户能直接改的文本**（也是 adb 调试时会改的东西），
     *   同样不可信。最要命的一项不是「值难看」，是 **`hiPct <= loPct`**：
     *   分位区间退化成一个点 → `lin = (bandDb - lo) / span` 里 span 退化成
     *   一个极小的数 → 归一化结果爆掉。宁可静默夹到边界，也不要带着越界值去跑。
     *
     * ⚠ 边界值和 Windows 版 `clampSettings()` **逐项一致**，不要各写各的。
     */
    fun clamp() {
        fun cl(v: Float, lo: Float, hi: Float) = if (v < lo) lo else if (v > hi) hi else v

        audioIntensity = cl(audioIntensity, 0.3f, 2.5f)
        responseRange = cl(responseRange, 0.3f, 2.0f)
        beamInset = cl(beamInset, -0.15f, 0.30f)
        themeCycleSec = cl(themeCycleSec, 10f, 300f)
        peakIntensity = cl(peakIntensity, 0f, 2f)
        idleDebounce = cl(idleDebounce, 0.5f, 5f)
        idleFade = cl(idleFade, 0.5f, 5f)
        alphaK = cl(alphaK, 0f, 1f)
        gainDb = cl(gainDb, -12f, 12f)
        widthDb = cl(widthDb, 15f, 60f)
        balance = cl(balance, 0f, 1f)
        contrast = cl(contrast, 1f, 3f)
        level = cl(level, 0.05f, 1f)
        loPct = cl(loPct, 0.02f, 0.50f)
        hiPct = cl(hiPct, 0.80f, 1.00f)
        rpWinSec = cl(rpWinSec, 3f, 30f)
        /* ★ hi 必须严格大于 lo，否则分位区间退化成一个点、归一化除零。 */
        if (hiPct <= loPct) hiPct = loPct + 0.10f

        mapMode = mapMode.coerceIn(0, 1)
        bandMode = bandMode.coerceIn(0, 1)
        styleMode = styleMode.coerceIn(0, 1)
        audioIdleMode = audioIdleMode.coerceIn(0, 1)
        themeIndex = themeIndex.coerceIn(0, LP_THEMES.size - 1)
        if (fpsLimit != 30 && fpsLimit != 60 && fpsLimit != 120 && fpsLimit != 144) fpsLimit = 60
        /* 分析频率只认这四个值。⚠ 落回默认时是 250，不是 60 ——
           60 是「慢 4.5 倍」的那个错值，绝不能当兜底。 */
        if (anaHz != 30 && anaHz != 60 && anaHz != 120 && anaHz != 250) anaHz = 250
        if (displayIndex < -1) displayIndex = -1
        /* 安卓特有：合成测试音已移除，2 是旧 ini 的遗留值 → 落回麦克风 */
        audioSourceIndex = audioSourceIndex.coerceIn(0, 1)
    }

    /** 把当前值摊平成一张 kv（用 PresetIo 的写方向），用于和预设逐键比对。 */
    fun snapshot(): LinkedHashMap<String, String> {
        val kv = LinkedHashMap<String, String>()
        val io = PresetIo(kv, true)
        visitAll(io)
        return kv
    }
}

/* ============================================================================
 *  参数 IO —— **一份清单，三个方向**
 *
 *  ⚠⚠⚠ 这里有一个曾经出过大事故的地方，动之前先读完。
 *
 *  `visitAll(io)` 是唯一的清单，但通信用的是**三个不同方向**：
 *
 *      IniIo        ini  ⇄ 内存      （saving 决定方向）
 *      PresetIo     预设kv ⇄ 内存    （saving 决定方向）
 *      PresetLoadIo  ini  → 预设kv   （★ 单向，第三方向）
 *
 *  安卓版第一版（对应 Windows 版 v8）曾经把「读预设文件」写成了
 *  `PresetIo(kv, saving = false)` —— 看着像「把文件读进 kv」，
 *  实际是「把**空 kv** 的默认值写回内存」。后果有两层：
 *
 *    ① loadPresets() 紧跟在 loadSettings() 后面跑 → 每次启动把用户设置
 *       整份冲成出厂默认值，然后 saveSettings() 又把这份默认值写回文件；
 *    ② 预设的 kv 从头到尾是空的 → 套用预设 = 把默认值铺上去。
 *       用户看到的就是「保存了但切回去没反应、退出重开全没了」。
 *
 *  教训：**「读」和「写」是两个不同的方向。** 共用一张 kv 的类只能覆盖
 *  kv ⇄ 内存这两个方向；ini → kv 必须是第三个类型，不能复用。
 *  不要给同一个类加一个 mode 枚举来省事。
 *
 *  第二条教训更贵：**从「出厂默认值」出发的自检，测不出「被冲成默认值」。**
 *  Windows 版 v8 的 12 项自检在有 bug 的版本上**全部通过**，因为
 *  「存了再套」的值一直在内存 kv 里（没经过读文件）、
 *  「写出去的值对不对」是直接读文件（也没经过读进内存）——
 *  bug 恰好藏在中间那一句。所以判据里必须有**哨兵值**：
 *  先把内存设成非默认值，再调 loadPresets()，看它有没有被动过。
 * ========================================================================== */

interface CfgIo {
    /** 写出去（true）还是读进来（false）。PresetLoadIo 用不到这个字段。 */
    var saving: Boolean
    fun visit(acc: KeyAcc)
}

/** ini ⇄ 内存。 */
class IniIo(private val file: File, override var saving: Boolean) : CfgIo {
    private val ini: IniFile = if (saving) IniFile.empty() else IniFile.load(file)

    override fun visit(acc: KeyAcc) {
        if (saving) {
            ini.set(SECTION, acc.name, acc.curStr())
        } else {
            acc.set(parseByType(acc.type, ini.get(SECTION, acc.name, acc.defStr()), acc.def))
        }
    }

    fun flush() {
        if (saving) ini.save(file)
    }

    companion object {
        const val SECTION = "app"
    }
}

/** 预设 kv ⇄ 内存。kv 是有序的，写出来键序和清单一致，人看着舒服。 */
class PresetIo(
    private val kv: LinkedHashMap<String, String>,
    override var saving: Boolean,
) : CfgIo {
    override fun visit(acc: KeyAcc) {
        if (saving) {
            kv[acc.name] = acc.curStr()
        } else {
            acc.set(parseByType(acc.type, kv[acc.name] ?: acc.defStr(), acc.def))
        }
    }
}

/**
 * ★ 第三方向：预设 ini → kv。**单向**，永远不碰内存。
 *
 * 和 PresetIo 的区别只有一个，但就是这一个决定了成败：
 * 它是从**文件**读，不是从 kv 读。
 */
class PresetLoadIo(
    private val kv: LinkedHashMap<String, String>,
    private val ini: IniFile,
    private val section: String,
) : CfgIo {
    /** 无意义：本类的方向由构造方式固定。接口要求实现，故留空。 */
    override var saving: Boolean = true

    override fun visit(acc: KeyAcc) {
        kv[acc.name] = ini.get(section, acc.name, acc.defStr())
    }
}
