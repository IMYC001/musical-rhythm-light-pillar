#!/usr/bin/env python3
"""从原作品 bundle 里**逐字**抠出音频分析类，生成 tools/orig-analyzer.js。

为什么不用「照着手抄一份」：手抄会引入我自己的理解偏差，而这次要查的恰恰是
「我的理解和原作到底差在哪」。所以直接按大括号配对把源码原样切出来，
一个字都不改，然后包一层导出。

抠两个类：
  · uC —— 节拍触发器（Pulse / Meteor），eG 的构造函数里 new 了它
  · eG —— 音频分析器：setWallpaperAudioData / getAudioData / evaluateTrigger

用法:
  python tools/extract_analyzer.py            # 写 tools/orig-analyzer.js
  python tools/extract_analyzer.py --check    # 只打印摘要，不写文件
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# 开源仓库里不带原作品的 bundle，所以没有「默认路径」这回事 ——
# 必须用 --bundle 或 LP_BUNDLE 指过去。
BUNDLE = ""
OUT = os.path.join(HERE, "orig-analyzer.js")


def resolve_bundle():
    """bundle 的路径，按优先级：--bundle <path> > 环境变量 LP_BUNDLE。

    `tools/orig-analyzer.js` 已经进版本库，所以日常构建不需要跑这一步。
    只有想「对着最新版原作品重新比对」时才需要指 bundle。
    """
    if "--bundle" in sys.argv:
        i = sys.argv.index("--bundle")
        if i + 1 < len(sys.argv):
            return os.path.abspath(sys.argv[i + 1])
    if os.environ.get("LP_BUNDLE"):
        return os.path.abspath(os.environ["LP_BUNDLE"])
    return BUNDLE


def slice_class(src, name):
    """从 `class <name>{` 开始按大括号配对切到收尾的 `}`。"""
    m = re.search(r"class\s+%s\s*\{" % re.escape(name), src)
    if not m:
        raise SystemExit("找不到 class %s" % name)
    i = src.index("{", m.start())
    depth = 0
    j = i
    while j < len(src):
        c = src[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[m.start():j + 1]
        elif c in "\"'`":
            # 跳过字符串/模板串，避免里面的花括号把配对搞乱
            q = c
            j += 1
            while j < len(src):
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == q:
                    break
                j += 1
        elif c == "/" and j + 1 < len(src) and src[j + 1] in "/*":
            if src[j + 1] == "/":
                j = src.find("\n", j)
                if j < 0:
                    break
            else:
                j = src.find("*/", j)
                if j < 0:
                    break
                j += 1
        j += 1
    raise SystemExit("class %s 的大括号没配平" % name)


def main():
    check_only = "--check" in sys.argv
    bundle = resolve_bundle()
    if not bundle:
        print("没有指定原作品 bundle。", file=sys.stderr)
        print("用 --bundle <index-*.js 的路径> 或环境变量 LP_BUNDLE 指过去。", file=sys.stderr)
        print("（tools/orig-analyzer.js 已进版本库，日常构建不需要跑这一步。）", file=sys.stderr)
        return 2
    if not os.path.isfile(bundle):
        print("找不到原作品 bundle：%s" % bundle, file=sys.stderr)
        print("用 --bundle <index-*.js 的路径> 或环境变量 LP_BUNDLE 指过去。", file=sys.stderr)
        return 2
    with open(bundle, encoding="utf-8") as f:
        src = f.read()
    print("bundle: %s" % bundle)

    uC = slice_class(src, "uC")
    eG = slice_class(src, "eG")

    print("uC: %d 字符" % len(uC))
    print("eG: %d 字符" % len(eG))
    for kw in ("getAudioData", "evaluateTrigger", "setWallpaperAudioData",
               "smoothedData", "prevData", "wallpaperAudioData"):
        print("  eG 含 %-22s %s" % (kw, kw in eG))
    # ee 的原始值 —— 本轮争议的中心，必须在提取结果里看见
    m = re.search(r"ee\s*=\s*[^,;]+", eG)
    print("  ee 原文: %s" % (m.group(0) if m else "!! 没找到"))
    # 上采样的那三行
    m = re.search(r"setWallpaperAudioData\(e\)\{.{0,320}", eG)
    print("  setWallpaperAudioData: %s" % (m.group(0) if m else "!! 没找到"))

    if check_only:
        return

    header = """/* 本文件由 tools/extract_analyzer.py 自动生成 —— 请勿手改。
 *
 * 内容是从原作品 bundle 里**逐字**切出来的两个类：
 *   uC —— 节拍触发器（Pulse / Meteor）
 *   eG —— 音频分析器（setWallpaperAudioData / getAudioData / evaluateTrigger）
 *
 * 它存在的唯一目的：当「原生分析链」的参照物。
 * 把同一串 128 个频谱值喂给它和 src/audiotest.cpp，逐帧比 16 个平滑量。
 * 因为是逐字提取，所以比对结果里不会有「我抄错了」这种噪声 ——
 * 差多少就是真的差多少。
 */
'use strict';

"""
    tail = """

/* ---- 导出：只导出分析器本体，其余保持原样 ---- */
module.exports = { AudioAnalyzer: eG, Trigger: uC };
"""
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        f.write(uC)
        f.write("\n\n")
        f.write(eG)
        f.write(tail)
    print("已写出 %s（%d 字节）" % (OUT, os.path.getsize(OUT)))


if __name__ == "__main__":
    sys.exit(main() or 0)
