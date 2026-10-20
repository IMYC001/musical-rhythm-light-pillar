#!/usr/bin/env python3
"""量一遍 main.cpp 里所有面板提示（addHint）的显示宽度，标出会被省略号截断的行。

为什么需要：面板是自绘的，提示用 `StringTrimmingEllipsisCharacter + NoWrap`，
**超宽就静默截断**（画面上表现为文字被切掉一半），既不换行也不报警。
而面板宽度只有 320px、可用 296px、字号 10.5px —— 换算下来
CJK 字符约 28 个、ASCII 约 53 个就到头了。写文案时全靠感觉，很容易超。

用法: python tools/check_hints.py [--all]
"""
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "main.cpp")

PANEL_W = 320.0
PAD = 12.0
FONT_PX = 10.5
LIMIT = (PANEL_W - PAD * 2) / FONT_PX * 2      # 以「半宽」为单位


def halfwidth(s):
    """一个 CJK/全角字符算 2 个半宽，ASCII 算 1 个。"""
    w = 0
    for ch in s:
        w += 2 if ord(ch) > 0x2000 else 1
    return w


def main():
    show_all = "--all" in sys.argv
    text = open(SRC, encoding="utf-8").read()
    # 抓 addHint( <tab>, { L"...", L"...", ... } );
    pat = re.compile(r"addHint\(\s*(\d)\s*,\s*\{(.*?)\}\s*\)\s*;", re.S)
    bad = 0
    total = 0
    for m in pat.finditer(text):
        tab = m.group(1)
        body = m.group(2)
        lines = re.findall(r'L"((?:[^"\\]|\\.)*)"', body)
        for ln in lines:
            total += 1
            w = halfwidth(ln)
            over = w > LIMIT
            if over:
                bad += 1
            if show_all or over:
                print("%s [tab %s] %5.1f/%d  %s" %
                      ("✗ 超宽" if over else "   ok ", tab, w, int(LIMIT), ln))
    print("-" * 70)
    print("共 %d 行提示，其中 %d 行会被截断（上限约 %.0f 半宽 = 28 个汉字）"
          % (total, bad, LIMIT))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
