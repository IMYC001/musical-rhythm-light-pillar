#!/usr/bin/env bash
# 「光柱离边距离」（beamInset）单点验证。
#
# 为什么要这么绕：beamInset 只在 bootstrap 时读一次盘，所以每换一个值都要
# 「改 ini → 重启 App → 点启动光柱 → 截图」，没法在界面上拖滑杆慢慢试
# （拖滑杆得到的坐标不可复现，量出来的几何也就没法互相比较）。
#
# 关键手法一：debugMode=12 把驱动量**钉死**在 0.90。
#   用真实音频的话 drive 在 0.004~0.26 之间跳，两次采样时刻不同就得出互相矛盾的
#   结论（本项目真机上吃过这个亏）。钉死之后画面不随时间变，量出来的几何才可比较。
#
# 关键手法二：截图**成对**取 —— 覆盖层开着一张、force-stop 之后一张。
#   两张相减，剩下的只有覆盖层画上去的光，壁纸本身的花纹不会混进来。
#
# ★★ 这个脚本第一版**给出了假阳性**：uiautomator dump 失败 → 按钮没点到 →
#    覆盖层压根没起来 → 两张截图当然一模一样 → 量出来「增量全 0」，
#    看着像「光柱消失」的成功结果，其实是「什么都没画」。
#    所以现在每一步都硬校验，任一步不成立就 exit 1，绝不输出几何结论。
#
# 用法: tools/bi_case.sh <beamInset> <标签>
#   例: tools/bi_case.sh -0.15 bi015
set -u

# 公共配置：adb / 设备 / python 全部走环境变量，见 _common.sh
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

INS="${1:-}"; TAG="${2:-}"
[ -n "$INS" ] && [ -n "$TAG" ] || die "用法: tools/bi_case.sh <beamInset> <标签>"

"$A" connect $DEV >/dev/null 2>&1

# ⓪ 屏幕必须亮着：灭屏时 screencap 全黑、uiautomator 也可能直接失败
"$A" -s $DEV shell svc power stayon true >/dev/null 2>&1
"$A" -s $DEV shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
sleep 1

# ① 本地生成 ini：只改 beamInset / debugMode / audioSourceIndex，其余照用户原配置
"$PY" - "$D/tmp/settings_orig.ini" "$D/tmp/bi_${TAG}.ini" "$INS" <<'EOF'
import sys, re
src, dst, ins = sys.argv[1], sys.argv[2], sys.argv[3]
t = open(src, encoding='utf-8').read()
t = re.sub(r'^beamInset=.*$',        'beamInset=' + ins,   t, flags=re.M)
t = re.sub(r'^debugMode=.*$',        'debugMode=12',       t, flags=re.M)
t = re.sub(r'^audioSourceIndex=.*$', 'audioSourceIndex=0', t, flags=re.M)
open(dst, 'w', encoding='utf-8').write(t)
EOF

"$A" -s $DEV shell am force-stop $PKG
sleep 1
"$A" -s $DEV logcat -c
"$A" -s $DEV push "$D/tmp/bi_${TAG}.ini" /data/local/tmp/s.ini >/dev/null
"$A" -s $DEV shell su -c "cp /data/local/tmp/s.ini /data/data/$PKG/files/settings.ini"
# 校验 ini 真的换上了 —— 换失败的话下面量的还是上一个档位，结论会张冠李戴
GOT=$("$A" -s $DEV shell su -c "grep ^beamInset= /data/data/$PKG/files/settings.ini" | tr -d '\r')
[ "$GOT" = "beamInset=$INS" ] || die "ini 没换成功：设备上是「$GOT」，期望「beamInset=$INS」"

# ② 起界面 → 界面树里定位「启动光柱」→ 点它
#
# ★★ 背景必须是**逐像素静止**的，这是整套取证里最容易忽略、也最贵的一条。
#    踩过三次，每次都先得到一张看着很合理的假证据：
#      ① 桌面当底 + 两次间隔 3 秒 → 桌面图标/文字还在重绘，差分里一片亮斑；
#      ② 桌面当底 + 两次都等 8 秒 → 桌面那个**大时钟**跳了一分钟（9:52 → 9:53），
#         差分里冒出白色数字，且不对称；
#      ③ 设置页顶端当底 → 设置页自己的状态行（○ 未启动 → ● 光柱运行中）
#         和按钮文案（启动光柱 → 停止光柱）必然变，又是一块假光。
#    最终方案：**设置页 + 滚到底**。
#      · 底部那几行（性能区的滑杆和说明）是纯静态的；
#      · 顶部的状态行/按钮已经滚出视口；
#      · 「滚到底」会被 ScrollView **夹住**，所以两次的滚动位置逐像素相同 ——
#        这一点是关键，随便滚一个中间位置是靠不住的（惯性滑动停在哪不好复现）。
#    注意覆盖层是**独立窗口**，滚动设置页不会移动光柱，光柱仍是整根可见的。
fling_bottom() {
  for i in $(seq 1 14); do
    "$A" -s $DEV shell input swipe 240 560 240 100 200
  done
  sleep 2
}
fling_top() {
  for i in $(seq 1 14); do
    "$A" -s $DEV shell input swipe 240 100 240 560 200
  done
  sleep 2
}

"$A" -s $DEV shell am start -n $PKG/.MainActivity >/dev/null
sleep 5
fling_bottom
"$A" -s $DEV exec-out screencap -p > "$D/tmp/bi_${TAG}_off.png"

fling_top
XY=""
for try in 1 2 3; do
  "$A" -s $DEV shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
  "$A" -s $DEV pull /data/local/tmp/ui.xml "$D/tmp/ui_${TAG}.xml" >/dev/null 2>&1
  if [ -s "$D/tmp/ui_${TAG}.xml" ]; then
    XY=$("$PY" "$D/tools/uitree.py" "$D/tmp/ui_${TAG}.xml" --find 启动光柱 2>/dev/null) && [ -n "$XY" ] && break
  fi
  XY=""
  echo "   (dump 第 $try 次没拿到，重试)"
  sleep 2
done
[ -n "$XY" ] || die "界面树里找不到「启动光柱」，放弃（不输出几何结论）"

echo "[$TAG] beamInset=$INS  启动按钮 @ $XY"
"$A" -s $DEV shell input tap ${XY% *} ${XY#* }

# ★ 硬校验：必须真的看到**新进程**的探针日志，否则说明覆盖层没起来
sleep 6
"$A" -s $DEV logcat -d -s LightPillar:I > "$D/tmp/log_${TAG}.txt"
"$PY" - "$D/tmp/log_${TAG}.txt" <<'EOF'
import sys
t = open(sys.argv[1], encoding='utf-8', errors='replace').read()
m = [l for l in t.splitlines() if 'probe max=' in l]
if not m:
    sys.exit("!! 没有探针日志 → 覆盖层没起来（很可能按钮没点到）")
print("   探针: " + m[-1].split('LightPillar: ', 1)[-1])
EOF
[ $? -eq 0 ] || exit 1

# 再滚回**同一个**夹紧位置截图。区别只在于覆盖层在跑。
fling_bottom
"$A" -s $DEV exec-out screencap -p > "$D/tmp/bi_${TAG}_on.png"

"$A" -s $DEV shell am force-stop $PKG

echo "── beamInset=$INS（$TAG）──"
"$PY" "$D/tools/pillar_metrics.py" "$D/tmp/bi_${TAG}_off.png" "$D/tmp/bi_${TAG}_on.png"
