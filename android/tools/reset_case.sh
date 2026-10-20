#!/usr/bin/env bash
# 「恢复默认设置」按钮的端到端验证。
#
# 判据（四条，缺一条就不算通过）：
#   ① 按钮在界面上存在，且位于「音频响应校准」之后、「预设」之前；
#   ② 点下去之后 settings.ini 里 27 个键**全部**变成出厂默认值；
#   ③ 两个「机器项」**没被动**：`[android] audioSourceIndex` 保持原值；
#   ④ 预设**一份没少**：presets.ini 里的档位还在（这是这个按钮敢不做二次确认的前提）。
#
# 外加一条界面判据：控件要**当场**刷回默认值。这条最容易漏 ——
# 参数其实已经变了、界面还停在旧值上，用户看到的就是「点了没反应」。
#
# 用法: tools/reset_case.sh
set -u
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

FILES=/data/data/$PKG/files

dump() {   # $1 = 本地文件名
  "$A" -s $DEV shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
  "$A" -s $DEV pull /data/local/tmp/ui.xml "$D/tmp/$1" >/dev/null 2>&1
  [ -s "$D/tmp/$1" ] || die "界面树没拿到（$1）"
}

"$A" connect $DEV >/dev/null 2>&1
"$A" -s $DEV shell svc power stayon true >/dev/null 2>&1
"$A" -s $DEV shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1

# ① 铺一个已知状态：用户原配置 + 一套与之完全一致的预设（这样下拉会显示它的名字）
"$A" -s $DEV shell am force-stop $PKG
sleep 1
"$A" -s $DEV push "$D/tmp/settings_orig.ini" /data/local/tmp/s.ini >/dev/null
"$A" -s $DEV push "$D/tmp/presets_test.ini" /data/local/tmp/p.ini >/dev/null
"$A" -s $DEV shell su -c "cp /data/local/tmp/s.ini $FILES/settings.ini"
"$A" -s $DEV shell su -c "cp /data/local/tmp/p.ini $FILES/presets.ini"

"$A" -s $DEV shell am start -n $PKG/.MainActivity >/dev/null
sleep 4
dump ui_reset_before.xml

# 设置页顶端（交付用截图：这一版把「音频来源」标题挪到控件上面了，得重拍）
"$A" -s $DEV exec-out screencap -p > "$D/交付/界面_设置页.png"

# ★ 往下滚：480×640 的屏上「恢复默认设置」在首屏之外，不滚的话点不到。
#   ⚠ 不能「一口气滚到底」——滚过头它就跑出视口上方了，一样找不到。
#     所以边滚边找，每次 200px，找到就停。
XY=""
for i in $(seq 1 16); do
  dump ui_reset_mid.xml
  XY=$("$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_mid.xml" --find 恢复默认设置 2>/dev/null) && [ -n "$XY" ] && break
  XY=""
  "$A" -s $DEV shell input swipe 240 560 240 360 250
  sleep 1
done
[ -n "$XY" ] || die "滚了 16 次也没找到「恢复默认设置」按钮"

# 再往下挪 200px：让按钮从视口底部升到中上部，这样交付截图里能**同时**看到
# 「恢复默认设置」和它下面的预设区 —— 一张图说清这两块的先后关系。
"$A" -s $DEV shell input swipe 240 560 240 360 250
sleep 1
dump ui_reset_mid.xml
XY=$("$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_mid.xml" --find 恢复默认设置) \
  || die "挪动之后按钮丢了（说明滚过头了）"

echo "══ ① 点击前（滚动后）的界面：应显示「测试档」= 预设已识别 ══"
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_mid.xml" 测试档
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_mid.xml" 恢复默认
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_mid.xml" 增益
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_mid.xml" 总电平
echo
echo "══ ② 按钮位置 = ($XY) ══"

"$A" -s $DEV exec-out screencap -p > "$D/交付/界面_恢复默认与预设.png"

"$A" -s $DEV shell input tap ${XY% *} ${XY#* }
sleep 2
dump ui_reset_after.xml
"$A" -s $DEV exec-out screencap -p > "$D/交付/界面_恢复默认之后.png"

echo
echo "══ ③ 点击后的界面（关键项应已回到默认值）══"
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_after.xml" 增益
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_after.xml" 光柱离边距离
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_after.xml" 音频响应强度
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_after.xml" 渲染帧率
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_after.xml" 频率轴
"$PY" "$D/tools/uitree.py" "$D/tmp/ui_reset_after.xml" 当前档位

echo
echo "══ ④ 落盘的 settings.ini ══"
"$A" -s $DEV shell su -c "cat $FILES/settings.ini"

echo
echo "══ ⑤ 落盘的 presets.ini（预设必须还在）══"
"$A" -s $DEV shell su -c "cat $FILES/presets.ini"
