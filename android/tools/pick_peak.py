"""在一批截图里找「左边缘光柱最高」的那张。

为什么需要它：光柱高度 = drive * 0.62，而测试音是**脉冲式**的（0.5s 一记鼓、
指数衰减），截图时刻落在鼓点间隙就几乎拍不到光柱。逐张肉眼挑效率太低，
所以按「左边缘若干列的亮度总和」排序 —— 总和最大的那张就是光柱最高/最亮的。

用法：python pick_peak.py shots/burst
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngtool import read, rgbrows  # noqa: E402

# 只看最左 8 列（光柱贴边；beamInset 默认 0）
COLS = 8


def score(path):
    w, h, ch, rows = read(path)
    rows = rgbrows(rows, ch)
    total = 0
    lit_rows = 0
    for y in range(h):
        r = rows[y]
        s = 0
        for x in range(min(COLS, w)):
            i = x * 3
            s += r[i] + r[i + 1] + r[i + 2]
        total += s
        # 单列平均 > 12/255 才算「发亮」（纯黑壁纸 + 加光，噪声远低于此）
        if s > 12 * 3 * COLS:
            lit_rows += 1
    return total, lit_rows, h


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    items = []
    for name in sorted(os.listdir(d)):
        if not name.lower().endswith(".png"):
            continue
        p = os.path.join(d, name)
        try:
            t, lit, h = score(p)
        except Exception as e:
            print("跳过 %s: %s" % (name, e))
            continue
        items.append((t, lit, h, name))
    items.sort(reverse=True)
    print("按左边缘亮度排序（越大 = 光柱越高越亮）：")
    for t, lit, h, name in items:
        print("  %-12s 总亮度=%-10d 发亮行数=%d/%d (%.1f%%)"
              % (name, t, lit, h, 100.0 * lit / h))
    if items:
        print("\n最佳：%s" % items[0][3])


if __name__ == "__main__":
    main()
