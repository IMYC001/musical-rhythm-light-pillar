#!/usr/bin/env bash
# 多分辨率验证：改 wm size/density → 重启应用 → 定位启动按钮 → 截图 → 检查覆盖层视口尺寸。
# 用法: multires_test.sh <WxH> <density> <标签>
set -u
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

SIZE="${1:-}"; DENS="${2:-}"; TAG="${3:-}"
[ -n "$SIZE" ] && [ -n "$TAG" ] || die "用法: multires_test.sh <WxH> <density> <标签>"

"$A" connect $DEV >/dev/null 2>&1
"$A" -s $DEV shell "wm size $SIZE; wm density $DENS" >/dev/null
sleep 2
"$A" -s $DEV shell "svc power stayon true; input keyevent KEYCODE_WAKEUP" >/dev/null
sleep 1

"$A" -s $DEV shell am force-stop com.lightpillar
sleep 1
"$A" -s $DEV shell am start -n com.lightpillar/.MainActivity >/dev/null
sleep 3
"$A" -s $DEV exec-out screencap -p > "$D/shots/mr_${TAG}_ui.png"

# 用像素分析定位「启动光柱」按钮的 y 中心（按钮是比背景亮的中灰块）
Y=$("$PY" - "$D/shots/mr_${TAG}_ui.png" "$D/tools" <<'EOF'
import sys
sys.path.insert(0, sys.argv[2])
from pngtool import read, rgbrows
w,h,ch,rows = read(sys.argv[1]); r = rgbrows(rows,ch)
cx = w//2
runs=[]; cur=None
for y in range(h):
    v = r[y][cx*3:cx*3+3]
    lum = sum(v)/3.0
    # 按钮灰：明显高于背景(11,11,18) 但低于纯白文字
    isbtn = 60 < lum < 130
    if isbtn and cur is None: cur=y
    elif not isbtn and cur is not None:
        if y-cur > 20: runs.append((cur,y-1))
        cur=None
if cur is not None and h-cur>20: runs.append((cur,h-1))
print(runs[0][0] + (runs[0][1]-runs[0][0])//2 if runs else int(h*0.22))
EOF
)
echo "[$TAG] $SIZE@$DENS  按钮 y=$Y"
"$A" -s $DEV shell input tap $(( $(echo $SIZE | cut -dx -f1) / 2 )) $Y
sleep 5
"$A" -s $DEV shell input keyevent KEYCODE_HOME
sleep 4
"$A" -s $DEV exec-out screencap -p > "$D/shots/mr_${TAG}.png"
"$A" -s $DEV logcat -d -s LightPillar:V | tail -1
echo "[$TAG] 截图 -> shots/mr_${TAG}.png"
