#!/usr/bin/env bash
# 诊断脚本的公共配置。被 bi_case.sh / reset_case.sh / dbg_run.sh / multires_test.sh source。
#
# 全部通过环境变量覆盖，没有硬编码的本机路径：
#
#   LP_ADB      adb 可执行文件      （默认：PATH 里的 adb）
#   LP_DEV      设备序列号           （默认：adb devices 里第一台）
#   LP_PYTHON   python 解释器        （默认：python3 / python）
#
# 例：
#   LP_DEV=192.168.1.50:5555 tools/bi_case.sh -0.15 bi015

# ★★ Git Bash 会把 `/data/local/tmp/...` 当成 Unix 路径**转换成本地路径**，
#    于是 `adb push` 报 `remote secure_mkdirs failed`、`uiautomator dump` 静默写到
#    别的地方去 —— 两者都**不会**让脚本停下来，只会让后面「按钮找不到」。
#    凡是从 bash 脚本调 adb 传设备路径，第一行就得关掉它。
export MSYS_NO_PATHCONV=1

A="${LP_ADB:-adb}"
D="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${LP_PYTHON:-$(command -v python3 || command -v python || echo python)}"

if ! command -v "$A" >/dev/null 2>&1 && [ ! -x "$A" ]; then
  echo "!! 找不到 adb：$A（用 LP_ADB=/path/to/adb 指定）" >&2
  exit 1
fi

DEV="${LP_DEV:-}"
if [ -z "$DEV" ]; then
  DEV="$("$A" devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
fi
if [ -z "$DEV" ]; then
  echo "!! 没有在线的设备（用 LP_DEV=<序列号> 指定；先 adb connect <ip:port>）" >&2
  exit 1
fi

PKG=com.lightpillar

die() { echo "!! $*" >&2; exit 1; }
