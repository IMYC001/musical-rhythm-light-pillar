#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「光柱离边距离」几个档位的取证截图拼成一张对照图。

为什么用**差分**而不是直接拼原图：截图里壁纸本身就有紫色渐变和亮点，
直接看「哪儿亮」分不清哪些是光柱、哪些是壁纸。有覆盖层/无覆盖层两张相减，
剩下的**只有**覆盖层画上去的光，底是纯黑 —— 光柱的形状一眼就能比。

为什么还要乘增益：默认「光柱实心度」是 0.10（纯加光，像真光打上去），
差分出来的峰值只有 50~180（0~255 刻度）。原样拼出来偏暗，乘 GAIN 再看更清楚。
⚠ 增益只影响观感，**不影响结论** —— 所有定量结论都来自 pillar_metrics.py 的原始差值。

用法: tools/bi_compose.py 出图.png 标签:目录前缀 ... 标签:目录前缀
  例: tools/bi_compose.py 交付/验证5_xxx.png "-0.15:tmp/bi_bi015" "-0.10:tmp/bi_bi010" ...
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngtool import read, rgbrows, write  # noqa: E402

GAIN = 2.2      # 差分放大倍数（纯观感）
# 噪声门限：≤FLOOR 的差值直接当 0。
# 为什么需要：两次截图即使滚到同一个夹紧位置，亚像素级的滚动偏移仍会让设置页的
# 文字抗锯齿差 1~2 个灰阶。乘上 GAIN 之后这些残影会隐约浮出来（像一层灰字），
# 容易和真正的光混淆。门限取 3 —— 和 pillar_metrics 的判据**同一个数**，
# 所以图上看到的边缘就是量出来的边缘，不会出现「图上更宽」这种不一致。
FLOOR = 3
# 裁掉顶部 280 行：
#   · 状态栏（时钟每分钟会跳，两张截图之间必然不一样）；
#   · 设置页底部那几行文字。它们本身**不在**光柱的列范围里是干净的
#     （实测「光柱以外增量>3 的像素 = 0 个」），但滚动到夹紧位置时的亚像素偏移
#     会让文字的抗锯齿在光柱**内部**的列上差几十个灰阶，视觉上就是光柱里浮出字。
#     裁掉它，图更干净，而光柱的主体（y=221..640）仍然完整。
CROP_TOP = 280
SEP = 8         # 面板之间的白色分隔条宽度
SEP_RGB = (255, 255, 255)


def load(p):
    w, h, ch, rows = read(p)
    return w, h, rgbrows(rows, ch)


def main():
    out = sys.argv[1]
    items = [a.split(':', 1) for a in sys.argv[2:]]
    assert items, "至少给一个 标签:目录前缀"

    panels = []
    for tag, pre in items:
        w, h, A = load(pre + '_off.png')     # 无覆盖层
        w2, h2, B = load(pre + '_on.png')    # 有覆盖层
        assert (w, h) == (w2, h2), "%s 两张图尺寸不一致" % tag
        rows = []
        for y in range(CROP_TOP, h):
            ra, rb = A[y], B[y]
            line = bytearray()
            for x in range(w):
                i = x * 3
                for c in range(3):
                    raw = rb[i + c] - ra[i + c]
                    d = 0 if raw <= FLOOR else int(raw * GAIN)
                    line.append(255 if d > 255 else d)
            rows.append(bytes(line))
        panels.append((tag, w, h - CROP_TOP, rows))
        print("  %-6s 裁切后 %dx%d" % (tag, w, h - CROP_TOP))

    W = sum(p[1] for p in panels) + SEP * (len(panels) - 1)
    H = max(p[2] for p in panels)
    canvas = []
    for y in range(H):
        line = bytearray()
        for k, (_t, pw, ph, rows) in enumerate(panels):
            line += rows[y] if y < ph else bytes(pw * 3)
            if k != len(panels) - 1:
                line += bytes(SEP_RGB) * SEP
        canvas.append(bytes(line))

    write(out, W, H, 3, canvas)
    print("→ %s  (%dx%d，顺序：%s)" % (out, W, H, " | ".join(p[0] for p in panels)))


if __name__ == "__main__":
    main()
