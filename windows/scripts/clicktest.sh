#!/usr/bin/env bash
# 用**真实的覆盖层**跑一次跨进程点击穿透测试。
#
# 顺序很关键：
#   ① 靶窗口必须先建 —— 它是 WS_EX_TOPMOST，保证不会被第三方窗口盖住
#   ② 覆盖层后建（同样置顶 → 创建更晚 → 压在靶上面）
#   ③ 覆盖层起来之后才放行注入点击
# 之前反过来做，结果点击经常被某个第三方窗口截胡，得出「两边都没收到」的
# 假象。顺序确定之后，结果就稳定可复现了。
#
# 用法: ./scripts/clicktest.sh <标签> [覆盖层参数...]
set -u
cd "$(dirname "$0")/.."

LABEL="${1:-test}"; shift || true
# ⚠ 必须用**相对路径**：Git Bash 的 /c/Users/... 形式 Windows 程序读不了
#   （会被当成 C:\c\Users\... 而打不开文件）。相对路径两边都认。
D="verify3/ct_$LABEL"
rm -rf "$D"; mkdir -p "$D"
RES="$D/result.txt"; READY="$D/ready.txt"; GO="$D/go.txt"

echo "── 测试 [$LABEL]  覆盖层参数: $* ──"

# ① 靶窗口先建（屏幕正中 240x160）
./build/clickprobe.exe 680 370 240 160 "$RES" "$READY" "$GO" > "$D/probe.log" 2>&1 &
CP=$!

# ② 等它就位
for _ in $(seq 1 120); do [ -f "$READY" ] && break; sleep 0.1; done
if [ ! -f "$READY" ]; then echo "  靶窗口没就绪，放弃"; kill $CP 2>/dev/null; exit 1; fi

# ③ 起覆盖层
./build/光柱.exe "$@" > "$D/overlay.log" 2>&1 &
OV=$!
sleep 4

# ④ 抓屏（确认画面还在）
./build/shot.exe "$D/screen.png" > /dev/null 2>&1

# ⑤ 放行注入
echo go > "$GO"

# ⑥ 等靶进程收工
wait $CP 2>/dev/null

if [ -f "$RES" ]; then
  echo "  靶窗口: $(cat "$RES")"
else
  echo "  靶窗口: (没写出结果)"
fi
echo "  覆盖层日志: $(grep -c . "$D/overlay.log" 2>/dev/null || echo 0) 行"

# 覆盖层靠 --hold 自己退；兜底再等一下
wait $OV 2>/dev/null
echo "  抓屏: $D/screen.png"
