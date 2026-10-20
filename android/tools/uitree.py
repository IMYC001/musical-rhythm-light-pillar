#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 uiautomator dump 出来的界面树按**屏幕上的先后顺序**打成一张清单。

为什么需要它：uiautomator 的 XML 是嵌套的，用 grep 找文本**看不出顺序**，
而本项目里出过好几次「控件的分组标题画到了控件下面」这种纯顺序 bug
（`Sec("音频来源")` 留在清单里、下拉在外面加，面板又是后 build 的）。
有了这个脚本，顺序问题变成一次字符串比对，不用靠肉眼看截图。

用法：
    python tools/uitree.py tmp/ui1.xml                  # 打全部
    python tools/uitree.py tmp/ui1.xml 恢复默认          # 只打命中的行（含上下各 2 行）
    python tools/uitree.py tmp/ui1.xml --find 启动光柱   # 只输出「x y」（给 adb input tap 用）
"""
import sys
import xml.etree.ElementTree as ET


def walk(node, out):
    """深度优先 = 屏幕上的先后顺序（LinearLayout 竖直排列时成立）。"""
    t = node.get("text", "")
    c = node.get("class", "")
    b = node.get("bounds", "")
    if t:
        out.append((t, c, b, node.get("clickable") == "true"))
    for ch in node:
        walk(ch, out)


def center(bounds):
    """'[0,100][480,200]' → (240, 150)"""
    a, bb = bounds.split("][")
    x1, y1 = a.lstrip("[").split(",")
    x2, y2 = bb.rstrip("]").split(",")
    return (int(x1) + int(x2)) // 2, (int(y1) + int(y2)) // 2


def main():
    path = sys.argv[1]
    find = sys.argv[2] == "--find" if len(sys.argv) > 2 else False
    needle = sys.argv[3] if find else (sys.argv[2] if len(sys.argv) > 2 else None)

    out = []
    walk(ET.parse(path).getroot(), out)

    if find:
        for t, _c, b, _ck in out:
            if needle in t:
                cx, cy = center(b)
                print("%d %d" % (cx, cy))
                return
        sys.exit("界面树里找不到「%s」" % needle)

    if needle:
        hit = [i for i, r in enumerate(out) if needle in r[0]]
        show = set()
        for i in hit:
            show.update(range(max(0, i - 2), min(len(out), i + 3)))
        idx = sorted(show)
    else:
        idx = range(len(out))

    for i in idx:
        t, c, b, ck = out[i]
        cx, cy = center(b)
        kind = c.rsplit(".", 1)[-1]
        flag = "  ←可点" if ck else ""
        print("%3d  %-14s %-22s %-12s 中心=(%d,%d)%s" % (i, kind, b, t, cx, cy, flag))


if __name__ == "__main__":
    main()
