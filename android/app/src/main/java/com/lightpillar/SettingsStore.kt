package com.lightpillar

import android.content.Context
import java.io.File

/**
 * 参数与预设的落盘管理。
 *
 * 文件布局（对应 Windows 版的 `%APPDATA%\LightPillar\`）：
 *   `settings.ini` 一份，随时被覆盖 —— 「我现在用的参数」
 *   `presets.ini`  多份，用户显式增删 —— 「我存下来的几套参数」
 *
 * 两者分开存是有意的：一个随时被覆盖的文件里混着多份用户资产，
 * 删一个档位就可能把当前设置一起删掉。
 *
 * ★★★ 启动顺序是这里最容易出事的地方，见 [bootstrap] 的注释。
 */
class SettingsStore(context: Context, baseDir: File = context.filesDir) {

    val settingsFile = File(baseDir, "settings.ini")
    val presetsFile = File(baseDir, "presets.ini")

    val settings = Settings()

    /** 每套预设：名字 + 一份有序 kv */
    class Preset(val name: String, val kv: LinkedHashMap<String, String>)

    val presets = ArrayList<Preset>()

    /** 下拉当前选中的下标 +1（0 = 未选中）。和 Windows 版一致。 */
    var selected = 0

    /** 启动时读进来的原始 ini（预设区要复用它，见下）。 */
    private var presetsIni: IniFile = IniFile.empty()

    /** 是否已经读过盘。见 [bootstrap] —— 重复调用不该再读一次。 */
    private var loaded = false

    /* ==================== 启动 ==================== */

    /**
     * ★★★ 启动顺序：先读 settings.ini，再读 presets.ini。
     *
     * ⚠ 第二步**绝不允许**碰 [settings] 里的任何值。
     *   这里曾经出过一次大事故（Windows 版 v8）：
     *   读预设时用了「kv ⇄ 内存」那个方向、而 kv 当时是空的，
     *   于是 visitAll 把每个键都当成「文件里没有」→ 把**默认值写回了内存**。
     *   后果是每次启动把用户设置整份冲成出厂默认值，
     *   然后 saveSettings() 又把这份默认值写回文件。
     *   用户看到的是「保存了但切回去没反应、退出重开全没了」。
     *
     *   所以 [loadPresets] 内部只用 [PresetLoadIo]（ini → kv，单向），
     *   一个字节都不写回 [settings]。
     *
     * ★ 幂等：只读一次盘。因为 Activity 和服务**共用同一个实例**（见 [SettingsHolder]），
     *   两边都会调 bootstrap()。第二次再读盘会把面板刚改、还在 350ms 防抖窗口里
     *   没落盘的值冲掉。
     *
     * @return 读到的预设数量，便于自检/日志
     */
    fun bootstrap(): Int {
        if (loaded) return presets.size
        // ① 读 settings.ini → 内存
        IniIo(settingsFile, saving = false).also { settings.visitAll(it) }
        // ② 安卓特有参数（[android] section），不在 27 键清单里
        val a = IniFile.load(settingsFile)
        settings.audioSourceIndex = a.get("android", "audioSourceIndex", "0").toIntOrNull() ?: 0
        settings.debugMode = a.get("android", "debugMode", "0").toIntOrNull() ?: 0
        /* ★ ③ 夹回合法范围。ini 是用户能直接改的文本，不可信 ——
           尤其是 hiPct <= loPct 会让归一化除零。 */
        settings.clamp()
        // ④ 读 presets.ini → 预设表（**不动** ③ 的结果）
        loadPresets()
        loaded = true
        return presets.size
    }


    fun saveSettings() {
        IniIo(settingsFile, saving = true).also {
            settings.visitAll(it)
            it.flush()
        }
        /* 安卓特有参数追加到同一个文件的 [android] section。
           ⚠ 这里必须**重新 load** 再写：IniIo 那个实例写的是它自己的一份内存副本，
             拿它去写 [android] 会把上面刚写好的 27 键整段冲掉（它那份里没有）。 */
        val ini = IniFile.load(settingsFile)
        ini.set("android", "audioSourceIndex", settings.audioSourceIndex.toString())
        ini.set("android", "debugMode", settings.debugMode.toString())
        ini.save(settingsFile)
    }

    /* ==================== 预设 ==================== */

    fun loadPresets() {
        presets.clear()
        presetsIni = IniFile.load(presetsFile)

        val count = presetsIni.get("meta", "count", "0").toIntOrNull() ?: 0
        for (i in 0 until count.coerceIn(0, 200)) {
            val sec = "preset$i"
            if (!presetsIni.hasSection(sec)) continue

            val kv = LinkedHashMap<String, String>()
            /* ★ 第三方向：从**文件**读进 kv。
               ⚠ 这里必须是 PresetLoadIo。写成 PresetIo(kv, saving=false) 就是
                 上面 [bootstrap] 注释里那起事故 —— 看着像「读文件进 kv」，
                 实际是「把空 kv 的默认值写回内存」。 */
            PresetLoadIo(kv, presetsIni, sec).also { settings.visitAll(it) }

            val name = presetsIni.get(sec, "name", "档位${i + 1}")
            presets.add(Preset(name, kv))
        }

        /* 下拉只在「当前参数确实等于那一套」时才停上去。
           ⚠ 以前只按 meta/active 恢复 → 出现「下拉显示着某套预设、参数却是默认值」，
             人看到这种不一致只能得出「预设没生效」。
           ⚠ 这个判断**只在启动时做一次**：会话内「选中 → 继续调参数」不能清选择位，
             否则「选中 → 调 → 保存覆盖」这条正常流程就断了。 */
        val act = presetsIni.get("meta", "active", "-1").toIntOrNull() ?: -1
        selected = if (act in presets.indices && matchesCurrent(act)) act + 1 else 0
    }

    /**
     * 当前参数是否等于第 [idx] 套预设。
     *
     * 跳过 `displayIndex` 与 `audioIdleMode`：这两项是**机器项**，
     * 不跟预设走（显示器编号换台机器就没意义；空闲模式是运行时状态）。
     */
    fun matchesCurrent(idx: Int): Boolean {
        if (idx !in presets.indices) return false
        val cur = settings.snapshot()
        val kv = presets[idx].kv
        if (kv.isEmpty()) return false
        for ((k, v) in cur) {
            if (k == "displayIndex" || k == "audioIdleMode") continue
            if (kv[k] != v) return false
        }
        return true
    }

    /** 保存为新档位。返回新档位下标，失败返回 -1。 */
    fun saveAs(name: String): Int {
        val n = name.trim()
        if (n.isEmpty()) return -1
        val kv = settings.snapshot()
        presets.add(Preset(n, kv))
        selected = presets.size
        writePresets()
        return presets.size - 1
    }

    /** 覆盖第 [idx] 套。**覆盖**而不是新建 —— 这是「保存参数」按钮的语义。 */
    fun overwrite(idx: Int): Boolean {
        if (idx !in presets.indices) return false
        val old = presets[idx]
        presets[idx] = Preset(old.name, settings.snapshot())
        writePresets()
        return true
    }

    fun rename(idx: Int, newName: String): Boolean {
        val n = newName.trim()
        if (idx !in presets.indices || n.isEmpty()) return false
        presets[idx] = Preset(n, presets[idx].kv)
        writePresets()
        return true
    }

    fun delete(idx: Int): Boolean {
        if (idx !in presets.indices) return false
        presets.removeAt(idx)
        if (selected > 0) selected = if (selected - 1 == idx) 0 else if (selected - 1 > idx) selected - 1 else selected
        writePresets()
        return true
    }

    /**
     * 套用第 [idx] 套。
     *
     * ★ 返回值的契约必须**干净**：`null` = 真的套用了，非 `null` = 一个字都没动。
     *   ⚠ 这里原来对「越界」返回的是 `null`，和「成功」撞在一起。调用方
     *     （[PresetPanel.pick]）拿到 `null` 就会往下走
     *     `store.presets[idx].name` —— 越界时直接 IndexOutOfBounds。
     *     现在越界也返回一条错误文案，调用方只需要判 `null` 就够了。
     *
     * 空档位同样拒绝，**不许**悄悄把默认值铺上去 ——
     * 那正是 v8 事故里用户看到的现象（「切预设 = 恢复默认」）。
     */
    fun apply(idx: Int): String? {
        if (idx !in presets.indices) return "档位不存在（第 ${idx + 1} 档）"
        val p = presets[idx]
        if (p.kv.isEmpty()) return "预设「${p.name}」是空的，已跳过"
        PresetIo(p.kv, saving = false).also { settings.visitAll(it) }
        /* ★ 套用预设后也要夹一次：预设文件同样是用户能改的文本。
           （Windows 版 clampSettings 的注释原话：「载入预设也走这里」。） */
        settings.clamp()
        selected = idx + 1
        return null
    }

    fun indexOfName(name: String): Int = presets.indexOfFirst { it.name == name }

    private fun writePresets() {
        val ini = IniFile.empty()
        ini.set("meta", "count", presets.size.toString())
        ini.set("meta", "active", (selected - 1).toString())
        for ((i, p) in presets.withIndex()) {
            val sec = "preset$i"
            ini.set(sec, "name", p.name)
            for ((k, v) in p.kv) ini.set(sec, k, v)
        }
        ini.save(presetsFile)
    }

    /** 名字重复时给个不冲突的后缀。 */
    fun uniqueName(base: String): String {
        if (indexOfName(base) < 0) return base
        var i = 2
        while (indexOfName("$base $i") >= 0) i++
        return "$base $i"
    }
}

/**
 * ★★★ 进程内**唯一**的 [SettingsStore]。
 *
 * 为什么必须是单例（这不是「设计模式洁癖」，是一个真实 bug 的修法）：
 *
 *   `MainActivity` 和 `OverlayService` 以前**各自 new 了一个** SettingsStore。
 *   面板改的是 Activity 那一份；渲染线程每帧读的是**服务**那一份 ——
 *   而服务那份是它 `onCreate` 时 bootstrap 出来的**快照**，之后再没人碰过。
 *   于是「改参数 → 光柱没反应，得停掉重开才生效」。
 *
 *   Activity 那边的 `syncService()` 想补救：重发一次 ACTION_START。
 *   但服务的 `onStartCommand` 里，`overlay != null` 时什么都不做 ——
 *   它**以为**参数是从 Activity 推过来的，Activity **以为**服务会自己去读。
 *   两边都以为对方会做，结果没人做。
 *
 * 共用一份之后，「实时生效」是自然结果而不是额外机制：
 *   面板的滑杆回调直接写进这个对象 → 渲染线程**下一帧**读到的就是新值
 *   （它本来就每帧 `store.settings`）。见 [OverlayService] 渲染循环里的 syncAnalyzer()。
 *
 * ⚠ Activity 和服务在**同一个进程**（清单里没声明 android:process），所以单例成立。
 *   如果哪天给服务加了 `android:process=":overlay"`，这个前提就破了 ——
 *   那时必须换成 Binder / 广播 / 文件监听之类的跨进程同步。
 */
object SettingsHolder {
    @Volatile private var inst: SettingsStore? = null

    /** 取（必要时创建并 bootstrap）。两个调用方拿到的**是同一个对象**。 */
    fun get(ctx: Context): SettingsStore {
        val cur = inst
        if (cur != null) return cur
        synchronized(this) {
            val again = inst
            if (again != null) return again
            val s = SettingsStore(ctx.applicationContext)
            s.bootstrap()
            inst = s
            return s
        }
    }
}
