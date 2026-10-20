#!/usr/bin/env bash
# 原生版构建脚本。
#
# 用 llvm-mingw（clang + mingw-w64 ucrt），**不需要 MSVC / Windows SDK**。
#
#   ./build.sh            # 主程序 + 全部诊断工具
#   ./build.sh app        # 只编主程序
#   ./build.sh tools      # 只编诊断工具
#   ./build.sh bugdrag    # 对照组：故意退回「用客户区坐标拖动」的旧写法
#   ./build.sh bugpreset  # 对照组：故意退回「用 PresetIo 读预设」的旧写法
#
# 工具链位置（按优先级）：
#   1. 环境变量 LP_MINGW
#   2. 常见安装位置（见下面的候选列表）
#   3. PATH 里的 x86_64-w64-mingw32-g++
set -euo pipefail

cd "$(dirname "$0")"

# ---- 找编译器 --------------------------------------------------------------
# llvm-mingw 在 Windows 上带 .exe 后缀，在 Linux/macOS 上没有 —— 两种都要认。
find_toolchain() {
  if [ -n "${LP_MINGW:-}" ]; then
    echo "$LP_MINGW"
    return
  fi
  for d in \
    /opt/llvm-mingw/bin \
    /usr/local/llvm-mingw/bin \
    "$HOME/llvm-mingw/bin" \
    "C:/llvm-mingw/bin" \
    "C:/Program Files/llvm-mingw/bin"
  do
    if [ -d "$d" ]; then echo "$d"; return; fi
  done
  # 退路：系统 PATH
  if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    dirname "$(command -v x86_64-w64-mingw32-g++)"
    return
  fi
  echo ""
}

MINGW="$(find_toolchain)"
if [ -z "$MINGW" ]; then
  cat >&2 <<'EOF'
找不到 llvm-mingw。

装一个（自带 clang + mingw-w64，不需要 MSVC）：
  https://github.com/mstorsjo/llvm-mingw/releases

然后指定位置：
  LP_MINGW=/path/to/llvm-mingw/bin ./build.sh
EOF
  exit 1
fi

SUFFIX=""
[ -x "$MINGW/x86_64-w64-mingw32-g++.exe" ] && SUFFIX=".exe"
CXX="$MINGW/x86_64-w64-mingw32-g++$SUFFIX"
WINDRES="$MINGW/x86_64-w64-mingw32-windres$SUFFIX"

if [ ! -x "$CXX" ]; then
  echo "找不到编译器: $CXX" >&2
  exit 1
fi

NODE="${LP_NODE:-$(command -v node || true)}"
PYTHON="${LP_PYTHON:-$(command -v python3 || command -v python || true)}"

COMMON=(-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter)
# ⚠ 不要加 -fno-exceptions：GDI+（shot.exe）内部会 throw，
#   关掉异常后那个 throw 变成 UB，实测表现为「无输出的段错误」。
# -static: 不依赖任何 mingw 运行时 dll，单文件绿色
# -s     : 去掉符号表
# --no-insert-timestamp: 让构建**字节可复现**。
#   默认 lld 会把「链接时刻」写进 PE 头 COFF.TimeDateStamp（偏移 e_lfanew+8，
#   4 字节），于是同一份源码两次构建 md5 不同 —— 实测差异恰好只有 2 个字节
#   （时间戳低 16 位），.text/.rdata/.rsrc 等七段 md5 全同。
#   这会让人误以为「交付的 exe 和源码对不上」。关掉之后，文档里写的 md5
#   才是一把真的尺子：md5 变了就说明代码真的变了。
LDFLAGS=(-static -s -mwindows -Wl,--no-insert-timestamp)
# d3dcompiler_47: 运行时编译 HLSL（系统自带这个 dll）
LIBS=(-ld3d11 -ldxgi -ldcomp -ld3dcompiler_47
      -lole32 -lmmdevapi -luuid -lshell32 -lcomctl32 -lshlwapi -lgdi32 -luser32
      -lgdiplus -ldwmapi
      -lwinmm -lpsapi)

mkdir -p build

# 主题表由 bundle 抽出，改了原作品就重新生成。
# src/data_themes.h 已进版本库，所以没有 bundle 也能编译 —— 只是不会更新。
if [ -n "${LP_REGEN:-}" ]; then
  if [ -n "$NODE" ]; then
    "$NODE" scripts/gen_data.js || echo "    ⚠ 主题表重新生成失败，沿用现有 src/data_themes.h" >&2
  else
    echo "    ⚠ 没找到 node，跳过主题表生成（沿用现有 src/data_themes.h）" >&2
  fi
fi

TARGET="${1:-all}"

if [ "$TARGET" = "all" ] || [ "$TARGET" = "tools" ]; then
  echo "==> 编译透明度探针 probe.exe"
  "$CXX" "${COMMON[@]}" -o build/probe.exe src/probe.cpp src/d3d.cpp -ldwmapi "${LDFLAGS[@]}" "${LIBS[@]}"

  echo "==> 编译对比工具 cmp.exe"
  "$CXX" "${COMMON[@]}" -o build/cmp.exe src/cmp.cpp -static -s -Wl,--no-insert-timestamp -lgdiplus -lgdi32 -luser32 -lshell32

  echo "==> 编译截图工具 shot.exe"
  "$CXX" "${COMMON[@]}" -o build/shot.exe src/shot.cpp -static -s -Wl,--no-insert-timestamp -lgdiplus -lgdi32 -luser32 -lshell32

  echo "==> 编译跨进程点击靶 clickprobe.exe"
  "$CXX" "${COMMON[@]}" -municode -o build/clickprobe.exe src/clickprobe.cpp -static -s -Wl,--no-insert-timestamp -luser32 -lgdi32

  echo "==> 编译测试音源 tonetest.exe"
  "$CXX" "${COMMON[@]}" -o build/tonetest.exe src/tonetest.cpp -static -s -Wl,--no-insert-timestamp -lwinmm -luser32

  echo "==> 编译音频链路诊断 audiodiag.exe"
  "$CXX" "${COMMON[@]}" -o build/audiodiag.exe src/audiodiag.cpp -static -s -Wl,--no-insert-timestamp -lole32 -lmmdevapi -luuid

  echo "==> 编译默认设备静音/音量夹具 setmute.exe"
  "$CXX" "${COMMON[@]}" -o build/setmute.exe src/setmute.cpp -static -s -Wl,--no-insert-timestamp -lole32 -lmmdevapi -luuid

  echo "==> 编译穿透实验台 hitlab.exe"
  "$CXX" "${COMMON[@]}" -municode -o build/hitlab.exe src/hitlab.cpp -static -s -Wl,--no-insert-timestamp -luser32 -lgdi32 -lshell32

  echo "==> 编译分层渲染验证 layertest.exe"
  "$CXX" "${COMMON[@]}" -municode -o build/layertest.exe src/layertest.cpp -static -s -Wl,--no-insert-timestamp -luser32 -lgdi32

  echo "==> 编译纯计时对照探针 gapwatch.exe（无 D3D，用来分开「我们卡」和「系统卡」）"
  "$CXX" "${COMMON[@]}" -o build/gapwatch.exe src/gapwatch.cpp -static -s -Wl,--no-insert-timestamp -luser32

  echo "==> 编译环境体检 rchk.exe（远程会话 / 分辨率 / 刷新率）"
  "$CXX" "${COMMON[@]}" -o build/rchk.exe src/rchk.cpp -static -s -Wl,--no-insert-timestamp -luser32 -lgdi32

  echo "==> 编译面板拖动验证 dragtest.exe"
  "$CXX" "${COMMON[@]}" -municode -o build/dragtest.exe src/dragtest.cpp -static -s -Wl,--no-insert-timestamp -luser32

  echo "==> 编译单行输入框测试台 prompttest.exe"
  "$CXX" "${COMMON[@]}" -municode -o build/prompttest.exe src/prompttest.cpp src/prompt.cpp \
    -static -s -Wl,--no-insert-timestamp -luser32 -lgdi32

  echo "==> 编译分析链对照台 audiotest.exe"
  "$CXX" "${COMMON[@]}" -o build/audiotest.exe src/audiotest.cpp src/audio.cpp \
    -static -s -Wl,--no-insert-timestamp -lole32 -lmmdevapi -luuid -lwinmm

  echo "==> 从原作品 bundle 逐字提取参考分析器 tools/orig-analyzer.js"
  # 找不到原作品 bundle 时不中断构建（沿用现有的 orig-analyzer.js），
  # 但要让人看见 —— 否则会拿着过期参照物去比对还以为在比最新的。
  if [ -n "$PYTHON" ]; then
    "$PYTHON" tools/extract_analyzer.py >/dev/null 2>&1 || \
      echo "    ⚠ 没能重新提取（找不到原作品 bundle？）—— 沿用现有的 tools/orig-analyzer.js" >&2
  fi

  ls -la build/probe.exe build/shot.exe build/tonetest.exe build/clickprobe.exe build/audiodiag.exe
fi

# 对照组：故意退回「用客户区坐标拖动」的旧写法，用来证明 dragtest.exe 抓得住那个 bug。
if [ "$TARGET" = "bugdrag" ]; then
  echo "==> 编译对照组（旧坐标基准）build/LightPillar-bugdrag.exe"
  "$WINDRES" -I assets -i assets/app.rc -o build/app.res
  "$CXX" "${COMMON[@]}" -municode -DLP_BUG_DRAG -o "build/LightPillar-bugdrag.exe" \
    src/main.cpp src/d3d.cpp src/audio.cpp src/panel.cpp src/prompt.cpp \
    build/app.res \
    "${LDFLAGS[@]}" "${LIBS[@]}"
  ls -la "build/LightPillar-bugdrag.exe"
fi

if [ "$TARGET" = "all" ] || [ "$TARGET" = "app" ]; then
  echo "==> 编译资源（图标 + 清单）"
  "$WINDRES" -I assets -i assets/app.rc -o build/app.res

  echo "==> 编译光柱主程序 LightPillar.exe"
  "$CXX" "${COMMON[@]}" -municode -o "build/LightPillar.exe" \
    src/main.cpp src/d3d.cpp src/audio.cpp src/panel.cpp src/prompt.cpp \
    build/app.res \
    "${LDFLAGS[@]}" "${LIBS[@]}"
  ls -la "build/LightPillar.exe"
fi

# 对照组：故意退回「用 PresetIo 去读预设文件」的旧写法（= 把默认值套进 g_set，
# 既冲掉用户设置、又让预设读不出值）。用来证明自检查得出这个 bug。
if [ "$TARGET" = "bugpreset" ]; then
  echo "==> 编译对照组（旧写法）build/LightPillar-bugpreset.exe"
  "$WINDRES" -I assets -i assets/app.rc -o build/app.res
  "$CXX" "${COMMON[@]}" -municode -DLP_BUG_PRESETLOAD -o "build/LightPillar-bugpreset.exe" \
    src/main.cpp src/d3d.cpp src/audio.cpp src/panel.cpp src/prompt.cpp \
    build/app.res \
    "${LDFLAGS[@]}" "${LIBS[@]}"
  ls -la "build/LightPillar-bugpreset.exe"
fi

echo "==> 完成"
