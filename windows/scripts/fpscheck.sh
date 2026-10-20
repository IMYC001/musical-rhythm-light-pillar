#!/usr/bin/env bash
# 帧率上限验证：同一个鼓点音源下，分别把 fpsLimit 设成 30/60/120/144，
# 看「画面内容变化次数/秒」是否跟着涨。
#
# 为什么看这个指标而不是看 fps：
#   用户报的是「帧率调节没用，每个选项观感都一样」。渲染帧率本身一直是好的，
#   问题出在**喂给着色器的数据每秒只更新 30 次** —— 内容不变，渲染再快也是重复帧。
#   所以要测的是内容变化率，不是帧率。
#   （统计时只比音频驱动的那 11 个量，不含 uTime，否则每帧都算「变了」。）
#
# ⚠ v6 起这一层的语义变了：频谱固定 30Hz、**分析固定 anaHz（默认 250）**、
#   渲染按 fpsLimit。所以内容变化率的上限是 min(anaHz, fpsLimit)：
#     30 fps  → 约 30 次/秒（渲染跟不上分析，每帧内容都变）
#     60 fps  → 约 60
#     120 fps → 约 120
#     144 fps → 约 144（分析有 250，喂得起）
#   仍然随 fpsLimit 单调上升 —— 这就是「帧率调节有效」的判据。
#
# ⚠ 必须真的在放声音：不放音环回收不到任何东西，内容变化率会全是 0，
#   看着像「帧率失效」。（不用管默认设备是否静音 —— 实测静音不影响环回。）
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=perfcheck
mkdir -p "$OUT"
CFG="/c/Users/202/AppData/Roaming/LightPillar/settings.ini"
cp "$CFG" "$OUT/settings.ini.bak" 2>/dev/null || true

for fps in 30 60 120 144; do
  echo "======================== fpsLimit=$fps ========================"
  sed -i "s/^fpsLimit=.*/fpsLimit=$fps/" "$CFG"
  ./build/tonetest.exe 15 >/dev/null 2>&1 &
  sleep 1.5
  ./build/光柱.exe --perf 12 2>&1 | grep -E "画面内容变化|帧率上限|样本"
  sleep 3
done

cp "$OUT/settings.ini.bak" "$CFG" 2>/dev/null || true
