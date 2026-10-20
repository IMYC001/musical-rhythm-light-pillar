#!/usr/bin/env python3
"""把「有覆盖层 / 无覆盖层」成对截图做差分，拼成带标签的网格对照图。

差分而不是直接拼原图：截图里的壁纸本身有渐变和亮点，直接看分不清哪些是光柱。
两张相减剩下的**只有**覆盖层画上去的光，底是纯黑，形状一眼能比。

用法:
    python docs/tools/compose_grid.py 出图.png --cols 3 --crop 180:640 \
        "-0.15:../lightpillar-android/tmp/bi_bi015" "0:../lightpillar-android/tmp/bi_bi000"

每个参数是 `标签:前缀`，前缀下需要有 `_off.png`（无覆盖层）和 `_on.png`（有）。
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

GAIN = 2.2      # 差分放大倍数（纯观感，不影响任何定量结论）
FLOOR = 3       # 噪声门限：≤3 的差值当 0，去掉文字抗锯齿的残影
FONT = "C:/Windows/Fonts/msyh.ttc"


def diff_panel(prefix, crop, median=0):
    a = np.asarray(Image.open(prefix + "_off.png").convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(prefix + "_on.png").convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        raise SystemExit("%s 两张图尺寸不一致：%s vs %s" % (prefix, a.shape, b.shape))
    d = np.clip(b - a, 0, None)
    d = np.where(d <= FLOOR, 0, np.clip(d * GAIN, 0, 255)).astype(np.uint8)
    y0, y1 = crop
    img = Image.fromarray(d[y0:y1])
    if median > 1:
        # 中值滤波：差分里偶尔会透出底图文字的抗锯齿残影（细笔画），
        # 光柱是大面积渐变不受影响，细笔画会被直接抹掉。
        from PIL import ImageFilter
        img = img.filter(ImageFilter.MedianFilter(median))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("items", nargs="+")
    ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--crop", default="0:0", help="裁切行范围 y0:y1（0:0 = 不裁）")
    ap.add_argument("--gap", type=int, default=10)
    ap.add_argument("--label-h", type=int, default=30)
    ap.add_argument("--median", type=int, default=0, help="中值滤波核（0/1 = 关，5 = 去文字残影）")
    args = ap.parse_args()

    y0, y1 = (int(v) for v in args.crop.split(":"))
    crop = (y0, y1) if y1 > y0 else (0, 10 ** 6)

    panels = []
    for it in args.items:
        tag, pre = it.split(":", 1)
        panels.append((tag, diff_panel(pre, crop, args.median)))

    pw, ph = panels[0][1].size
    rows = (len(panels) + args.cols - 1) // args.cols
    cell_h = ph + args.label_h
    W = args.cols * pw + (args.cols - 1) * args.gap
    H = rows * cell_h + (rows - 1) * args.gap

    canvas = Image.new("RGB", (W, H), (16, 16, 20))
    font = ImageFont.truetype(FONT, 17)
    for i, (tag, img) in enumerate(panels):
        r, c = divmod(i, args.cols)
        x = c * (pw + args.gap)
        y = r * (cell_h + args.gap)
        canvas.paste(img, (x, y))
        d = ImageDraw.Draw(canvas, "RGBA")
        d.rectangle([x, y, x + pw - 1, y + ph - 1], outline=(58, 60, 74, 255))
        d.text((x + pw // 2, y + ph + args.label_h // 2), tag,
               font=font, fill=(232, 232, 240, 255), anchor="mm")

    canvas.save(args.out)
    print("→ %s  (%dx%d, %d 格)" % (args.out, W, H, len(panels)))


if __name__ == "__main__":
    main()
