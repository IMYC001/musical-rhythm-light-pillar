package com.lightpillar

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import kotlin.math.roundToInt

/**
 * 预设区：下拉选档位 + 保存 / 覆盖 / 重命名 / 删除。
 *
 * ★ 语义对齐 Windows 版（那里踩过一次，这里照抄结论）：
 *   · 「保存当前参数」在**已选中某档位**时是**覆盖**那一档，未选中时才新建；
 *   · 下拉只在「当前参数确实等于那一套」时才显示它 —— 不许说谎；
 *   · 套用前先保存当前参数，免得用户「调了半天没存就切走」。
 */
class PresetPanel(
    private val act: Activity,
    private val store: SettingsStore,
    private val root: LinearLayout,
    private val onChanged: () -> Unit,
) {
    private lateinit var cur: TextView

    private fun dp(v: Int) = (v * act.resources.displayMetrics.density).roundToInt()

    private fun toast(s: String) = Toast.makeText(act, s, Toast.LENGTH_SHORT).show()

    fun build() {
        root.addView(TextView(act).apply {
            text = "预设"
            setTextColor(Color.parseColor("#C9B6FF"))
            textSize = 13f
            typeface = Typeface.DEFAULT_BOLD
            setPadding(0, dp(20), 0, dp(6))
        })

        val head = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL }
        val name = TextView(act).apply {
            text = "当前档位"
            setTextColor(Color.parseColor("#E8E8F0"))
            textSize = 14f
        }
        cur = TextView(act).apply {
            setTextColor(Color.parseColor("#9FE1CB"))
            textSize = 14f
            gravity = Gravity.END
        }
        head.addView(name, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(cur)

        val box = LinearLayout(act).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14), dp(10), dp(14), dp(10))
            addView(head)
            isClickable = true
            setOnClickListener { pick() }
        }
        root.addView(box)

        /* ---- 按钮行 ---- */
        val row = LinearLayout(act).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, dp(8), 0, 0)
        }
        row.addView(btn("保存参数") { onSave() }, weight())
        row.addView(btn("重命名") { onRename() }, weight())
        row.addView(btn("删除") { onDelete() }, weight())
        root.addView(row)

        root.addView(TextView(act).apply {
            text = "「保存参数」在已选中档位时是覆盖它；未选中时新建一档。"
            setTextColor(Color.parseColor("#8A8A99"))
            textSize = 11.5f
            setPadding(0, dp(4), 0, 0)
        })

        refresh()
    }

    private fun weight() = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        .apply { marginEnd = dp(6) }

    private fun btn(t: String, f: () -> Unit) = Button(act).apply {
        text = t
        textSize = 13f
        setOnClickListener { f() }
    }

    /**
     * 把「当前档位」这一行刷成 store 里的状态。
     *
     * ★ 对外公开（原来是 private）：MainActivity 的「恢复默认设置」要调它 ——
     *   恢复默认会把 [SettingsStore.selected] 清成 0（参考实现对应
     *   `activePreset = null`，注释原话「已经离开任何一套预设了，下拉框不该还显示着它」），
     *   不刷的话这一行还停在某一档的名字上，而参数已经是出厂默认值了 ——
     *   下拉在说谎，用户只会得出「预设被改坏了」。
     */
    fun refresh() {
        val sel = store.selected
        cur.text = if (sel in 1..store.presets.size) {
            store.presets[sel - 1].name
        } else {
            "（未选择）"
        }
    }

    /* ==================== 选档位 ==================== */

    private fun pick() {
        val labels = ArrayList<String>()
        labels.add("（未选择）")
        for (p in store.presets) labels.add(p.name)

        AlertDialog.Builder(act)
            .setTitle("选择档位")
            .setSingleChoiceItems(labels.toTypedArray(), store.selected) { d, which ->
                d.dismiss()
                if (which == 0) {
                    store.selected = 0
                    refresh()
                    return@setSingleChoiceItems
                }
                val idx = which - 1
                /* ⚠ 套用前先把当前参数落盘：用户可能「调了半天没存就切走」，
                   下一档又切回来时不该丢掉刚才那些调整。 */
                store.saveSettings()
                val err = store.apply(idx)
                if (err != null) {
                    toast(err)
                } else {
                    store.saveSettings()
                    onChanged()
                    refresh()
                    toast("已套用「${store.presets[idx].name}」")
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /* ==================== 保存 / 覆盖 ==================== */

    private fun onSave() {
        store.saveSettings()
        val sel = store.selected
        if (sel in 1..store.presets.size) {
            val idx = sel - 1
            AlertDialog.Builder(act)
                .setTitle("覆盖「${store.presets[idx].name}」？")
                .setMessage("会用当前参数覆盖这一档。")
                .setPositiveButton("覆盖") { _, _ ->
                    store.overwrite(idx)
                    onChanged()
                    refresh()
                    toast("已覆盖")
                }
                .setNeutralButton("存为新档位") { _, _ -> askName(null) }
                .setNegativeButton("取消", null)
                .show()
        } else {
            askName(null)
        }
    }

    private fun askName(renameIdx: Int?) {
        val ed = EditText(act).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            hint = "档位名字"
            setSingleLine()
            setText(if (renameIdx != null) store.presets[renameIdx].name else "")
        }
        AlertDialog.Builder(act)
            .setTitle(if (renameIdx != null) "重命名" else "存为新档位")
            .setView(ed)
            .setPositiveButton("确定") { _, _ ->
                val raw = ed.text.toString().trim()
                if (raw.isEmpty()) {
                    toast("名字不能为空")
                    return@setPositiveButton
                }
                if (renameIdx != null) {
                    store.rename(renameIdx, store.uniqueName(raw))
                    toast("已重命名")
                } else {
                    store.saveAs(store.uniqueName(raw))
                    toast("已保存")
                }
                onChanged()
                refresh()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun onRename() {
        val sel = store.selected
        if (sel !in 1..store.presets.size) {
            toast("先选一个档位")
            return
        }
        askName(sel - 1)
    }

    private fun onDelete() {
        val sel = store.selected
        if (sel !in 1..store.presets.size) {
            toast("先选一个档位")
            return
        }
        val idx = sel - 1
        AlertDialog.Builder(act)
            .setTitle("删除「${store.presets[idx].name}」？")
            .setMessage("只删这一档，当前参数不受影响。")
            .setPositiveButton("删除") { _, _ ->
                store.delete(idx)
                refresh()
                toast("已删除")
            }
            .setNegativeButton("取消", null)
            .show()
    }
}
