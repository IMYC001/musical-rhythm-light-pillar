#!/usr/bin/env bash
# 预设功能自检：必须在**有声**环境下跑 —— 预设里含「声音」页的参数，
# 不放声音的话 level / bandMode 那几项取证没有意义。
#
# 自检会把 presets.ini 写到输出目录（g_presetPathOverride），
# **不碰** %APPDATA%\LightPillar\presets.ini。
#
# ⚠ 必须先清掉残留的 光柱.exe：它占着单实例互斥体（LP_MUTEX_NAME），
#   之后每一个新实例都会弹一个「光柱已经在运行了」的消息框并**永远阻塞** ——
#   表现是「自检跑不完、输出目录空空如也」，看着像自检代码有死循环，
#   实际是上一次被信号打断留下的孤儿进程。用 `timeout`/被工具 SIGTERM 打断
#   最容易留下它（前台子进程不会跟着 bash 一起死）。
set -uo pipefail
cd "$(dirname "$0")/.."
OUT="build/stpreset"

# ---- 前置清理：干掉残留实例 ----
stray=$(tasklist /FO CSV /NH 2>/dev/null | iconv -f GBK -t UTF-8 2>/dev/null \
        | grep -a '"光柱.exe"' | awk -F'","' '{print $2}' || true)
if [ -n "${stray:-}" ]; then
  echo "⚠ 发现残留的 光柱.exe（PID: $stray），先结束它（它占着单实例互斥体）"
  for pid in $stray; do taskkill /F /PID "$pid" >/dev/null 2>&1 || true; done
  sleep 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

# 22 秒鼓点音源，覆盖整个自检窗口
./build/tonetest.exe 22 >/dev/null 2>&1 &
TONE=$!
sleep 1.5

./build/光柱.exe --selftest 22 "$OUT" 2>&1 | tail -5

wait "$TONE" 2>/dev/null || true
echo "=== 预设相关字段 ==="
grep -o '"参数预设": {[^}]*}' "$OUT/自检报告.json"
echo "=== 输出目录 ==="
ls "$OUT"
