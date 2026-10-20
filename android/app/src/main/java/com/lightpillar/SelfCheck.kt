package com.lightpillar

import android.content.Context
import android.util.Log
import java.io.File
import kotlin.math.abs

/**
 * 自检：把「曾经真的踩过坑的地方」变成可自动跑的判据。
 *
 * ★ 为什么要做成随包交付的开关，而不是写在开发笔记里：
 *   这套移植里出过的 bug，**没有一个**能靠「看一眼程序跑起来没崩」发现 ——
 *   参数被冲成默认值、预设有键无值、下拉显示与实际不符、ini 尾零导致跨平台假差异，
 *   全都表现为「程序正常运行」。没有判据就只能靠用户报障。
 *
 * ★★ 判据必须有**哨兵值**，不能从出厂默认值出发。
 *   Windows 版 v8 那次事故：读预设时把每个键都当成「文件里没有」→ 把默认值写回内存，
 *   于是每次启动把用户设置整份冲成出厂默认值。当时的 12 项自检**全部通过** ——
 *   因为那些判据都是从「默认值」出发的：存了再套（值一直在内存 kv 里）、
 *   写出去的值对不对（直接读文件、没经过读进内存那一步）。bug 恰好藏在中间那句话里。
 *   所以这里的做法是：**先把内存设成非默认值，再调 loadPresets()，看它有没有被动过**。
 *
 * ★★★ 触发方式：`settings.ini` 的 `[android] debugMode=100`，然后启动一次应用。
 *   报告写到 `filesDir/自检报告.json`，可用
 *   `adb exec-out run-as com.lightpillar cat files/自检报告.json` 取回。
 *
 * ⚠ 全程使用 `cacheDir/selftest/` 下的临时文件，**绝不碰**用户真实的
 *   settings.ini / presets.ini —— 自检把用户配置改坏了，比不检查还糟。
 */
object SelfCheck {

    class Item(val name: String, val pass: Boolean, val detail: String)

    /** 27 键的期望清单（键名/类型/默认值）。**逐字**对应 Windows 版的 visitAllSettings。 */
    private val EXPECTED: List<Triple<String, KType, Double>> = listOf(
        Triple("styleMode", KType.INT, 0.0),
        Triple("beamInset", KType.FLOAT, 0.0),
        Triple("themeIndex", KType.INT, 0.0),
        Triple("themeAuto", KType.BOOL, 0.0),
        Triple("themeCycleSec", KType.FLOAT, 60.0),
        Triple("audioIntensity", KType.FLOAT, 1.0),
        Triple("responseRange", KType.FLOAT, 1.0),
        Triple("peakEnabled", KType.BOOL, 1.0),
        Triple("peakIntensity", KType.FLOAT, 1.0),
        Triple("idleEnabled", KType.BOOL, 1.0),
        Triple("idleDebounce", KType.FLOAT, 1.0),
        Triple("idleFade", KType.FLOAT, 1.0),
        Triple("alphaK", KType.FLOAT, 0.10),
        Triple("fpsLimit", KType.INT, 60.0),
        Triple("anaHz", KType.INT, 250.0),
        Triple("displayIndex", KType.INT, -1.0),
        Triple("gainDb", KType.FLOAT, 0.0),
        Triple("widthDb", KType.FLOAT, 46.0),
        Triple("balance", KType.FLOAT, 0.85),
        Triple("mapMode", KType.INT, 0.0),
        Triple("bandMode", KType.INT, 0.0),
        Triple("contrast", KType.FLOAT, 1.8),
        Triple("level", KType.FLOAT, 0.15),
        Triple("loPct", KType.FLOAT, 0.20),
        Triple("hiPct", KType.FLOAT, 0.99),
        Triple("rpWinSec", KType.FLOAT, 10.0),
        Triple("audioIdleMode", KType.INT, 0.0),
    )

    fun run(ctx: Context): List<Item> {
        val out = ArrayList<Item>()
        fun chk(name: String, pass: Boolean, detail: String = "") = out.add(Item(name, pass, detail))

        val dir = File(ctx.cacheDir, "selftest").apply { mkdirs() }
        dir.listFiles()?.forEach { it.delete() }

        /* ============ A. 27 键清单 ============ */
        val s0 = Settings()
        chk("键数 = 27", s0.keys.size == 27, "实际 ${s0.keys.size}")
        chk(
            "键名无重复",
            s0.keys.map { it.name }.toSet().size == s0.keys.size,
            s0.keys.map { it.name }.groupBy { it }.filter { it.value.size > 1 }.keys.toString()
        )

        val mism = StringBuilder()
        for ((n, t, d) in EXPECTED) {
            val k = s0.byName[n]
            if (k == null) { mism.append("缺键:$n "); continue }
            /* ⚠ 模板里的 `${n}` 花括号不能省：中文是**合法的 Kotlin 标识符字符**，
               写成 `$n类型应为` 会被整个解析成一个叫「n类型应为」的标识符 → 编译错。 */
            if (k.type != t) mism.append("${n}类型应为${t}实际${k.type} ")
            /* ⚠ 容差必须按 **Float** 给：清单里是 `0.10f`，`Float→Double` 会带出
               1e-8 量级的表示误差（0.1f → 0.10000000149011612）。
               用 1e-9 比对会把 6 个键全判成「默认值不对」—— 假警报比不检查更糟，
               它会让人开始怀疑真判据。 */
            if (abs(k.def - d) > 1e-6) mism.append("${n}默认应为${d}实际${k.def} ")
        }
        for (k in s0.keys) if (EXPECTED.none { it.first == k.name }) mism.append("多出键:${k.name} ")
        chk("键名/类型/默认值逐项对照", mism.isEmpty(), mism.toString())

        /* 逐键可读可写（含非默认值，防止「只能读默认值」的假通过） */
        var rw = ""
        for (k in s0.keys) {
            val probe = when (k.type) {
                KType.INT -> 7.0
                KType.FLOAT -> 0.375
                KType.BOOL -> 1.0
            }
            k.set(probe)
            val back = k.get()
            if (abs(back - probe) > 1e-6) rw += "${k.name}(写${probe}读${back}) "
        }
        chk("27 键逐个可读可写", rw.isEmpty(), rw)

        /* ============ B. ini 格式（跨平台逐键对照的前提） ============ */
        chk(
            "fmtG 去尾零（C 的 %.6g 语义）",
            fmtG(0.1f) == "0.1" && fmtG(1.0f) == "1" && fmtG(0.85f) == "0.85" &&
                fmtG(46f) == "46" && fmtG(0f) == "0",
            "0.1→${fmtG(0.1f)} 1.0→${fmtG(1.0f)} 0.85→${fmtG(0.85f)} 46→${fmtG(46f)} 0→${fmtG(0f)}"
        )

        /* ============ C. 三个方向的 IO ============ */
        val f1 = File(dir, "settings.ini")
        val a = Settings()
        a.gainDb = -3.5f; a.themeIndex = 4; a.idleEnabled = false; a.balance = 0.37f
        IniIo(f1, saving = true).also { a.visitAll(it); it.flush() }

        val b = Settings()
        IniIo(f1, saving = false).also { b.visitAll(it) }
        chk(
            "IniIo 存→读 往返一致（含非默认值）",
            b.gainDb == -3.5f && b.themeIndex == 4 && !b.idleEnabled && b.balance == 0.37f,
            "gain=${b.gainDb} theme=${b.themeIndex} idle=${b.idleEnabled} bal=${b.balance}"
        )
        val ini1 = IniFile.load(f1)
        chk("ini 段落名 = app", ini1.hasSection("app"), ini1.sectionNames().toString())
        chk("ini 里恰好 27 个键", ini1.keyNames("app").size == 27, "实际 ${ini1.keyNames("app").size}")
        chk(
            "写盘后无残留 .tmp",
            File(dir, "settings.ini.tmp").exists().not(),
            "存在 settings.ini.tmp"
        )

        /* 哨兵：先把内存设成非默认值，再走一遍「读 ini → 内存」，看是否只改了文件里有的键 */
        val c = Settings()
        c.gainDb = -9f; c.contrast = 2.7f          // 哨兵值
        IniIo(f1, saving = false).also { c.visitAll(it) }
        chk(
            "IniIo 读入后内存 = 文件值（哨兵被覆盖）",
            c.gainDb == -3.5f && c.contrast == 1.8f,
            "gain=${c.gainDb} contrast=${c.contrast}"
        )

        /* ============ D. 预设：三方向里的「第三方向」 ============ */
        val store = SettingsStore(ctx, dir)
        // 造一份预设文件：两套，第 2 套与「出厂默认值」相同（用来测下拉诚实性）
        store.settings.gainDb = -6f
        store.settings.themeIndex = 3
        store.saveAs("甲")
        store.settings.gainDb = 0f
        store.settings.themeIndex = 0
        store.saveAs("乙")
        chk("保存两套预设", store.presets.size == 2, "实际 ${store.presets.size}")
        chk("预设落盘（presets.ini 存在）", store.presetsFile.exists(), store.presetsFile.path)

        /* ★★ 核心哨兵判据：loadPresets() 不许碰内存里的当前设置 */
        store.settings.gainDb = -11f
        store.settings.themeIndex = 9
        store.settings.alphaK = 0.42f
        val before = store.settings.snapshot()
        store.loadPresets()
        val after = store.settings.snapshot()
        val touched = before.filter { (k, v) -> after[k] != v }.keys.toString()
        chk("★★ loadPresets 不动内存里的当前设置", before == after, "被改动的键: $touched")
        chk("重新读预设后数量仍为 2", store.presets.size == 2, "实际 ${store.presets.size}")

        /* 预设内容本身要读得出来（v8 的第二个症状：有档位、无值） */
        val p0 = store.presets.getOrNull(0)
        chk(
            "预设内容读得出来（不是空 kv）",
            p0 != null && p0.kv.size == 27 && p0.kv["gainDb"] == fmtG(-6f),
            "档位0: 键数=${p0?.kv?.size} gainDb=${p0?.kv?.get("gainDb")}"
        )

        /* 套用预设：内存应变成该档的值 */
        val r = store.apply(0)
        chk(
            "套用预设 → 内存变为该档的值",
            r == null && store.settings.gainDb == -6f && store.settings.themeIndex == 3,
            "返回=$r gain=${store.settings.gainDb} theme=${store.settings.themeIndex}"
        )

        /* 越界档位：必须返回错误。**不能**返回 null —— 调用方拿到 null 就会
           往下走 presets[idx].name，越界时直接崩。 */
        val savedGain = store.settings.gainDb
        val savedTheme = store.settings.themeIndex
        val r2 = store.apply(store.presets.size)
        chk(
            "越界档位返回错误而非「成功」",
            r2 != null && store.settings.gainDb == savedGain && store.settings.themeIndex == savedTheme,
            "返回=${r2 ?: "null(会被当成套用成功!)"}"
        )

        /* 空档位（存在、但 kv 是空的）：拒绝，且**不许悄悄铺默认值**。
           直接往表里塞一个空 kv 的档位来测 —— 走文件造不出来：
           PresetLoadIo 会给段里缺的键填默认值，kv 永远不会是空的。 */
        store.presets.add(SettingsStore.Preset("空档", LinkedHashMap()))
        val r3 = store.apply(store.presets.size - 1)
        chk(
            "空档位拒绝套用且不铺默认值",
            r3 != null && store.settings.gainDb == savedGain && store.settings.themeIndex == savedTheme,
            "返回=${r3 ?: "null(未拒绝!)"} gain=${store.settings.gainDb} theme=${store.settings.themeIndex}"
        )
        store.presets.removeAt(store.presets.size - 1)

        /* 下拉诚实性：当前参数与第 2 套（= 出厂默认）一致时，selected 应指向它 */
        store.settings.gainDb = 0f
        store.settings.themeIndex = 0
        store.saveSettings()
        val store2 = SettingsStore(ctx, dir)
        store2.bootstrap()
        chk(
            "下拉诚实性：参数等于某档 → 选中它",
            store2.selected == 2,
            "selected=${store2.selected}（期望 2）"
        )
        store2.settings.gainDb = -4f
        store2.saveSettings()
        val store3 = SettingsStore(ctx, dir)
        store3.bootstrap()
        chk("下拉诚实性：参数不等于任何档 → 显示未选中(0)", store3.selected == 0, "selected=${store3.selected}")

        /* 启动顺序整体：bootstrap 不许把设置冲成默认值 */
        val store4 = SettingsStore(ctx, dir)
        store4.settings.gainDb = -8f
        store4.saveSettings()
        val store5 = SettingsStore(ctx, dir)
        store5.bootstrap()
        chk("bootstrap 读回刚保存的设置", store5.settings.gainDb == -8f, "gain=${store5.settings.gainDb}")

        /* [android] 段不许污染 27 键 */
        val ini5 = IniFile.load(File(dir, "settings.ini"))
        chk(
            "[android] 段存在且 app 段仍为 27 键",
            ini5.hasSection("android") && ini5.keyNames("app").size == 27,
            "段: ${ini5.sectionNames()} app键数=${ini5.keyNames("app").size}"
        )

        /* ============ E. 分析链 ============ */
        val an = Analyzer()
        an.sampleRate = 48000.0
        an.channels = 2
        an.anaHz = 250.0
        an.gainDb = 0.0
        an.widthDb = 46.0
        an.mapMode = 0
        an.bandMode = 0
        an.rpWinSec = 10.0
        an.paramsChanged()
        chk("bandSignature 对同一状态稳定", an.bandSignature() == an.bandSignature(), "")

        /* 频率轴：等宽 / 等比必须是**两张不同的**频段划分表。
           ★ Windows 版 v7 的 bug 正是「切了没反应」——控件值变了、
             频段划分表没重建，于是两个选项观感完全一样。 */
        an.bandMode = 0; an.paramsChanged(); val sigLin = an.bandSignature()
        an.bandMode = 1; an.paramsChanged(); val sigLog = an.bandSignature()
        chk(
            "频率轴 等宽/等比 → 划分表真的不同",
            sigLin != sigLog,
            "linear=$sigLin log=$sigLog"
        )
        an.bandMode = 0; an.paramsChanged()

        /* 跟踪窗口：帧数必须按 **30Hz**（频谱层频率）换算，不是渲染帧率。
           ★ 10s → 300 帧、4s → 120 帧，两个数都对才说明这条规则没写错。 */
        an.rpWinSec = 10.0; an.paramsChanged(); val w10 = an.trackWindowFrames
        an.rpWinSec = 4.0; an.paramsChanged(); val w4 = an.trackWindowFrames
        chk(
            "跟踪窗口按 30Hz 换算（10s→300 / 4s→120 帧）",
            w10 == 300 && w4 == 120,
            "10s=$w10 4s=$w4"
        )
        an.rpWinSec = 10.0; an.paramsChanged()

        /* 喂一段和测试音同结构的信号（0.5s 一记 60+100Hz 鼓、指数衰减）。
           ★★★ 必须喂满 **10 秒**，不能只喂 1 秒。
           为什么（这条是实测出来的，第一版自检就栽在这里）：
             perband 映射是 `lin = (bandDb - lo) / (hi - lo)`，lo/hi 取**该段自己的
             滚动直方图**的 20% / 99% 分位，窗口 = rpWinSec × 30Hz = **300 帧**。
             而 `lin` 是在**每个样本到达的那一刻**用当时的直方图算出来的，**事后不会重算**。
             只喂 1 秒 ≈ 30 个样本时直方图几乎是空的 → 20% 分位已经贴着峰值 →
             峰值 lin 只有 0.15 → 而 `spectrum = lin^contrast(1.8) × level(0.15)`，
             于是 subBass 峰值只有 0.0048，看着像「低频段坏了」。
             喂满 10 秒后同一个信号给出 subBass ≈ 0.09，和原生版自检报告的 0.093 对上。
           ★ 这**不是 bug**，是原作品的设计（10 秒滚动窗）—— 但由此可以读出一条
             真实世界的结论：**启动后头 10 秒动态范围是压缩的**，
             光柱偏矮、响应偏钝，10 秒后才进入满量程。诊断时别把它当成故障。

           ★★ 取的是**过程中的最大值**，不是喂完那一刻的值：
             鼓点每 0.5 秒一记、`exp(-phase*12)` 衰减，喂到整秒时正好落在衰减尾巴上
             （env ≈ exp(-6) ≈ 0.0025）。这正是真机上「截图常拍到没有光柱」的原因 ——
             测试音大部分时间处于脉冲间隙，采样时刻不同就得出互相矛盾的结论。 */
        val n = 48000 * 10          // 10 秒，喂满滚动直方图
        val pcm = FloatArray(n * 2)
        for (i in 0 until n) {
            val t = i.toDouble() / 48000.0
            val kp = t % 0.5
            val v = (Math.exp(-kp * 12.0) * (0.80 * Math.sin(2 * Math.PI * 60.0 * t) +
                0.35 * Math.sin(2 * Math.PI * 100.0 * t)) * 0.671)
            pcm[i * 2] = v.toFloat(); pcm[i * 2 + 1] = v.toFloat()
        }
        val maxSeen = DoubleArray(16)
        val step = 48000 / 250          // 按 anaHz 喂，不能用渲染帧率
        var fed = step
        while (fed <= n) {
            an.tick(1.0 / 250.0, pcm, fed)
            val s = an.smoothed()
            for (i in 0 until 16) if (s[i] > maxSeen[i]) maxSeen[i] = s[i]
            fed += step
        }
        /* 16 个量的下标（= 原作品 smoothedData 的键顺序）：
           0 bass / 1 mid / 2 treble / 3 energy / 4 subBass / 5 lowMid /
           6 highMid / 7 presence / 8 brilliance / 9 air / 10 warmth /
           11 brightness / 12 sharpness / 13 smoothness / 14 density / 15 centroid */
        val pushed = maxSeen.count { it > 0.01 }
        chk(
            "分析链：鼓点把 16 个平滑量推动起来",
            pushed >= 4,
            "被推动 ${pushed}/16，峰值 " + maxSeen.joinToString(" ") { "%.3f".format(it) }
        )
        chk(
            "分析链：subBass / bass 峰值明显高于零（鼓点确实打到低频段）",
            maxSeen[4] > 0.05 && maxSeen[0] > 0.05,
            "subBass=%.4f bass=%.4f（原生版自检实测 subBass=0.093 bass=0.066）"
                .format(maxSeen[4], maxSeen[0])
        )

        /* resetTrackers 只该重置滚动直方图，**不许**动平滑量 ——
           否则用户一改「跟踪窗口」，画面会整体跳一下。 */
        val keep = an.smoothed().copyOf()
        an.resetTrackers()
        chk(
            "resetTrackers 不清零平滑量（改参数时画面不跳）",
            an.smoothed().contentEquals(keep),
            "改前 " + keep.take(4).joinToString { "%.3f".format(it) } +
                " 改后 " + an.smoothed().take(4).joinToString { "%.3f".format(it) }
        )

        return out
    }

    /** 跑自检并把结果写成 JSON + 日志。返回 (通过数, 总数)。 */
    fun runAndReport(ctx: Context): Pair<Int, Int> {
        val items = try {
            run(ctx)
        } catch (e: Exception) {
            Log.e("LightPillar", "自检异常", e)
            listOf(Item("自检过程抛异常", false, e.toString()))
        }
        val pass = items.count { it.pass }
        val sb = StringBuilder()
        sb.append("{\n  \"通过\": $pass,\n  \"总数\": ${items.size},\n  \"项\": [\n")
        items.forEachIndexed { i, it ->
            sb.append("    {\"名称\": \"${esc(it.name)}\", \"结果\": ${it.pass}, \"详情\": \"${esc(it.detail)}\"}")
            sb.append(if (i < items.size - 1) ",\n" else "\n")
        }
        sb.append("  ]\n}\n")
        try {
            File(ctx.filesDir, "自检报告.json").writeText(sb.toString(), Charsets.UTF_8)
        } catch (e: Exception) {
            Log.e("LightPillar", "写自检报告失败", e)
        }
        for (it in items) {
            Log.i("LightPillar", (if (it.pass) "PASS " else "FAIL ") + it.name +
                (if (it.detail.isNotEmpty()) "  [${it.detail}]" else ""))
        }
        Log.i("LightPillar", "自检完成: $pass/${items.size} 通过")
        return pass to items.size
    }

    private fun esc(s: String): String =
        s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", " ")
}
