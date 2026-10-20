package com.lightpillar

import java.io.File
import java.util.Locale

/**
 * 与 Windows 版**同格式**的 ini 读写：[section] 下的 key=value。
 *
 * 为什么不用 java.util.Properties：
 *   1. Properties 没有 section 概念，而预设文件的结构是
 *      `[meta] count/active` + `[preset0] name/…`，section 是必须的；
 *   2. Windows 版用的是 `WritePrivateProfileStringW`，写出来就是这个格式。
 *      格式一致，两个平台的配置文件才能互相看懂、才能做逐键对照。
 *
 * ⚠ 写盘用「临时文件 + 改名」：直接往目标文件写，写到一半掉电或被系统杀
 *   会留下半个文件 —— 下次启动读到一个空配置，用户看到的是「设置全没了」。
 *   安卓上应用被 LMK 杀是常态，这个保护不是可选的。
 */
class IniFile private constructor(
    private val data: LinkedHashMap<String, LinkedHashMap<String, String>>,
) {
    fun get(section: String, key: String, def: String): String =
        data[section]?.get(key) ?: def

    fun set(section: String, key: String, value: String) {
        data.getOrPut(section) { LinkedHashMap() }[key] = value
    }

    /** 整段删除。预设「删除」用它 —— 和 Windows 版一样整段删，不留残键。 */
    fun removeSection(section: String) {
        data.remove(section)
    }

    fun hasSection(section: String): Boolean = data.containsKey(section)

    fun sectionNames(): List<String> = data.keys.toList()

    /** 某段里的键名（自检用：数「27 键是否一个不少」、查有没有多余键）。 */
    fun keyNames(section: String): List<String> = data[section]?.keys?.toList() ?: emptyList()

    fun save(file: File) {
        file.parentFile?.mkdirs()
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.bufferedWriter(Charsets.UTF_8).use { w ->
            for ((sec, kv) in data) {
                w.write("[$sec]")
                w.newLine()
                for ((k, v) in kv) {
                    w.write("$k=$v")
                    w.newLine()
                }
            }
        }
        if (file.exists() && !file.delete()) {
            tmp.delete()
            return
        }
        if (!tmp.renameTo(file)) {
            tmp.copyTo(file, overwrite = true)
            tmp.delete()
        }
    }

    companion object {
        fun load(file: File): IniFile {
            val data = LinkedHashMap<String, LinkedHashMap<String, String>>()
            try {
                if (file.exists()) {
                    var cur = ""
                    file.forEachLine(Charsets.UTF_8) { raw ->
                        val line = raw.trim()
                        if (line.isNotEmpty() && !line.startsWith(";") && !line.startsWith("#")) {
                            if (line.startsWith("[") && line.endsWith("]")) {
                                cur = line.substring(1, line.length - 1).trim()
                                data.getOrPut(cur) { LinkedHashMap() }
                            } else {
                                val i = line.indexOf('=')
                                if (i > 0) {
                                    val k = line.substring(0, i).trim()
                                    val v = line.substring(i + 1).trim()
                                    data.getOrPut(cur) { LinkedHashMap() }[k] = v
                                }
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                // 读坏了就当空配置，绝不让它把启动流程打断
                android.util.Log.w("LightPillar", "读 ini 失败: ${file.name}", e)
            }
            return IniFile(data)
        }

        fun empty(): IniFile = IniFile(LinkedHashMap())
    }
}

/**
 * 复刻 C 的 `%.6g`。
 *
 * ⚠ Java 的 `%g` 和 C 的 `%g` **不一样**：Java 会保留尾零（`0.85` → `"0.850000"`），
 *   C 不会（`0.85` → `"0.85"`）。Windows 版写出来的是后者。
 *   不去尾零的话两边文件看着不同、逐键对照时全是假差异。
 */
fun fmtG(v: Float): String {
    val s = String.format(Locale.US, "%.6g", v)
    val ei = s.indexOfFirst { it == 'e' || it == 'E' }
    return if (ei >= 0) {
        s.substring(0, ei).trimEnd('0').trimEnd('.') + s.substring(ei)
    } else {
        val t = s.trimEnd('0').trimEnd('.')
        if (t.isEmpty() || t == "-") "0" else t
    }
}

/** 按清单里声明的类型解析。解析不出来就取默认值（不抛异常）。 */
fun parseByType(type: KType, raw: String, def: Double): Double = when (type) {
    KType.INT -> raw.toIntOrNull()?.toDouble() ?: def
    KType.FLOAT -> raw.toFloatOrNull()?.toDouble() ?: def
    KType.BOOL -> if (raw == "1" || raw.equals("true", true)) 1.0 else 0.0
}
