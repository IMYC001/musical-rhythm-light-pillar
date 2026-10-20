#!/usr/bin/env bash
# 面板拖动验证：对照组（旧坐标基准）与修复版，各跑几个场景。
#
# 三个坑，都踩过：
#  ① **单实例**。程序启动时抢一个命名互斥量，抢不到就弹一个**模态对话框**并
#     一直占着互斥量。脚本里只要出现两个实例重叠，后面每一轮都会弹一个
#     看不见的对话框、把所有后续测试全毒掉（表现是「找不到窗口」）。
#     所以：每轮跑完必须等它自己退干净再开下一轮。
#  ② **存活时间**。--panelhold 到期程序自己退出，句柄一失效 GetWindowRect 就
#     失败、结构体保持全 0 —— 会被读成「窗口跑到 (0,0) 了」，看着像拖动把
#     窗口甩飞，其实只是被拖的对象没了。
#     Sleep(1) 在 Windows 上实测约 15ms，200 步的拖动要 3 秒才走完，
#     所以存活时间给 12 秒、轮与轮之间空 6 秒。
#  ③ 沙箱里不能从 Bash 调 PowerShell 去杀进程，taskkill 的 //F 也被挡 ——
#     干脆不杀，靠时间错开。
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=dragcheck
mkdir -p "$OUT"
DRAG=./build/dragtest.exe
TITLE=LpSettingsPanel
CFG="/c/Users/202/AppData/Roaming/LightPillar/settings.ini"

wait_clean() {   # 等上一个实例退干净（最多 25 秒）
  for _ in $(seq 1 25); do
    n=$(tasklist 2>/dev/null | grep -c "光柱" || true)
    [ "$n" = "0" ] && return 0
    sleep 1
  done
  echo "!! 还有光柱实例没退出，本轮作废（避免触发单实例模态框）"
  return 1
}

set_fps() {   # set_fps <30|60|120|144>
  [ -f "$CFG" ] || return 0
  sed -i "s/^fpsLimit=.*/fpsLimit=$1/" "$CFG"
}

run() {   # run <标签> <exe> <位移> <步进ms> <轨迹> [fps]
  local tag="$1" exe="$2" travel="$3" step="$4" trace="$5" fps="${6:-}"
  echo "==================== $tag ===================="
  wait_clean || return 1
  [ -n "$fps" ] && set_fps "$fps"
  "$exe" --panelhold 12 >/dev/null 2>&1 &
  sleep 5
  "$DRAG" "$TITLE" "$travel" "$step" "$OUT/$trace"
  echo "（退出码 $?）"
  sleep 6
}

# 备份用户配置，跑完还原
cp "$CFG" "$OUT/settings.ini.bak" 2>/dev/null || true

run "对照组 A：旧写法 · 慢拖 200px" build/光柱-bugdrag.exe 200 1 bug-A-slow.csv 60
run "对照组 B：旧写法 · 猛拖 300px（一次排空整批消息）" build/光柱-bugdrag.exe 300 0 bug-B-burst.csv 60
run "对照组 C：旧写法 · 30fps 下慢拖 200px（每帧攒更多消息）" build/光柱-bugdrag.exe 200 1 bug-C-30fps.csv 30
run "修复版 A：屏幕坐标 · 慢拖 200px" build/光柱.exe 200 1 fix-A-slow.csv 60
run "修复版 B：屏幕坐标 · 猛拖 300px（一次排空整批消息）" build/光柱.exe 300 0 fix-B-burst.csv 60
run "修复版 C：屏幕坐标 · 斜向 200x150（竖直也要跟）" build/光柱.exe 200x150 1 fix-C-diag.csv 60
run "修复版 D：屏幕坐标 · 30fps 下慢拖 200px" build/光柱.exe 200 1 fix-D-30fps.csv 30
run "修复版 E：屏幕坐标 · 144fps 下猛拖 400px" build/光柱.exe 400 0 fix-E-144fps-burst.csv 144

cp "$OUT/settings.ini.bak" "$CFG" 2>/dev/null || true

echo "==================== 轨迹文件 ===================="
ls -la "$OUT"/*.csv
