#!/usr/bin/env bash
# 一条命令跑完「原生分析链 vs 原作品分析链」的对照。
#
# 用法:
#   ./scripts/specdiff.sh                    # 用合成 dump（不需要音频设备）
#   ./scripts/specdiff.sh <dump.csv> [label] # 用真实 dump（--dumpspec 录的）
#
# 做四件事：
#   1. 从原作品 bundle 逐字重新提取参考分析器（保证参照物是当前 bundle）
#   2. 对 30/60/120/250 Hz 各跑一遍**原生**侧（audiotest.exe）
#   3. 对 250 Hz 跑一遍**原作品**侧（jsref.js）—— 250 是桌面版实测频率
#   4. 逐帧 diff，并列出各频率下的包络峰值
set -uo pipefail
cd "$(dirname "$0")/.."

PY="${LP_PYTHON:-$(command -v python3 || command -v python || echo python)}"
NODE="${LP_NODE:-$(command -v node || echo node)}"
OUT=cmp1
mkdir -p "$OUT"

DUMP="${1:-}"
LABEL="${2:-}"
if [ -z "$DUMP" ]; then
  DUMP="$OUT/synth.csv"
  echo "== 没有指定 dump，先造一份合成的（鼓点 + 中频 + 镲，12 秒）=="
  "$PY" tools/make_synth_dump.py "$DUMP" 12
fi
[ -f "$DUMP" ] || { echo "找不到 dump: $DUMP" >&2; exit 1; }

echo "== 从原作品 bundle 逐字提取参考分析器 =="
"$PY" tools/extract_analyzer.py

echo
echo "== 原生侧：30 / 60 / 120 / 250 Hz =="
for hz in 30 60 120 250; do
  ./build/audiotest.exe "$DUMP" "$OUT/native-$hz.csv" $hz "$LABEL"
done

echo
echo "== 原作品侧：250 Hz（= 桌面版实测频率）=="
"$NODE" tools/jsref.js "$DUMP" "$OUT/orig-250.csv" 250 "$LABEL"

echo
echo "== 逐帧比对（250 Hz）=="
"$PY" tools/specdiff.py "$OUT/native-250.csv" "$OUT/orig-250.csv"
RC=$?

echo
echo "== 各分析频率下的包络峰值（看「幅度差多少」）=="
"$PY" tools/specdiff.py --peaks "$OUT/native-30.csv" "$OUT/native-60.csv" \
       "$OUT/native-120.csv" "$OUT/native-250.csv"

exit $RC
