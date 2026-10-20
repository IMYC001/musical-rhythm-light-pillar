"""光柱几何/保真度量：拿「有覆盖层」和「无覆盖层」两张截图相减，
把光柱的贡献单独隔离出来，再量它的形状。

为什么必须相减而不是直接看截图：
  桌面壁纸本身在底部就有紫色渐变，直接按「亮不亮」数行数会把壁纸算进去
  （实测：壁纸贡献 60+ 行，光柱只有 40 行，混在一起分不清）。
  两张同机位、同壁纸的截图相减，剩下的**只有**覆盖层画上去的东西。

用法：python pillar_metrics.py shots/cmp_off.png shots/cmp_on.png [顶部裁切行数]

⚠ 顶部裁切不是可选项，是必须的：状态栏（时间、信号、电量图标）在两张截图之间
  会变，差值比光柱还大（实测最亮点落在状态栏上，增量 244）。
  默认裁掉 48 行。要连状态栏一起量就传 0。
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngtool import read, rgbrows  # noqa: E402

THRESH = 3   # 单通道差值超过它才算「光柱画到这里」（PNG 无损，噪声远小于此）


def load(p):
    w, h, ch, rows = read(p)
    return w, h, rgbrows(rows, ch)


def main():
    aw, ah, A = load(sys.argv[1])
    bw, bh, B = load(sys.argv[2])
    assert (aw, ah) == (bw, bh), "两张图尺寸不一致"
    w, h = aw, ah
    TOP = int(sys.argv[3]) if len(sys.argv) > 3 else 48

    def delta(x, y):
        i = x * 3
        ra, rb = A[y], B[y]
        return (abs(ra[i] - rb[i]) + abs(ra[i + 1] - rb[i + 1]) + abs(ra[i + 2] - rb[i + 2])) // 3

    # ---- ① 每列的最大增量 → 光柱横向射程 ----
    colmax = [0] * w
    for x in range(w):
        m = 0
        for y in range(TOP, h):
            d = delta(x, y)
            if d > m:
                m = d
        colmax[x] = m

    # ---- ② 每行的最大增量（左半 / 右半分开）→ 光柱纵向高度 ----
    def rowmax(x0, x1):
        out = [0] * h
        for y in range(TOP, h):
            m = 0
            for x in range(x0, x1):
                d = delta(x, y)
                if d > m:
                    m = d
            out[y] = m
        return out

    lm = rowmax(0, w // 2)
    rm = rowmax(w // 2, w)

    def top_lit(prof):
        """从底部往上，最高一个「被点亮」的行号（屏幕坐标，y 向下）。"""
        for y in range(TOP, h):
            if prof[y] > THRESH:
                return y
        return None

    lt, rt = top_lit(lm), top_lit(rm)

    print("图像 %dx%d（顶部裁掉 %d 行状态栏）" % (w, h, TOP))
    print()
    print("── ① 纵向：光柱从底部升到哪一行 ──")
    if lt is None:
        print("  左侧：没有检测到光柱")
    else:
        print("  左侧：顶部 y=%d，高度 %d px = 屏高的 %.1f%%" % (lt, h - lt, 100.0 * (h - lt) / h))
    if rt is None:
        print("  右侧：没有检测到光柱")
    else:
        print("  右侧：顶部 y=%d，高度 %d px = 屏高的 %.1f%%" % (rt, h - rt, 100.0 * (h - rt) / h))
    if lt is not None and rt is not None:
        print("  左右高度差 = %d px %s" % (abs((h - lt) - (h - rt)), "✓ 对称" if abs(lt - rt) <= 4 else "✗ 不对称"))

    print()
    print("── ② 横向：光柱落在哪些列上（每列最大增量）──")
    # ⚠ 这里**不能**写成「从 x=0 往右数到第一列变暗」——那个写法默认光柱贴着屏幕边。
    #   一旦「光柱离边距离」> 0（轴心被挪进画面里），最外几列本来就是暗的，
    #   数出来会是 0 px，看着像「光柱没了」，其实是量法只对 inset=0 成立。
    #   所以改成把**所有**被点亮的列扫出来、并成若干条「光带」，任何 inset 都能量。
    runs = []
    for x in range(w):
        if colmax[x] > THRESH:
            if runs and x == runs[-1][1] + 1:
                runs[-1][1] = x
            else:
                runs.append([x, x])

    if not runs:
        print("  没有任何一列被点亮 —— 光柱完全离场")
    for a, b in runs:
        side = "左" if b < w // 2 else ("右" if a >= w // 2 else "★跨中线")
        print("  %s光带 x=%d..%d（%d px = 屏宽的 %.1f%%）" % (side, a, b, b - a + 1, 100.0 * (b - a + 1) / w))

    if len(runs) == 2:
        (a1, b1), (a2, b2) = runs
        gap = a2 - b1
        print("  两柱内边缘间距 = %d px（左柱右缘 x=%d → 右柱左缘 x=%d）%s"
              % (gap, b1, a2, "✓ 中间留空" if gap > 1 else "✗ 两柱相连/重叠"))
        dl, dr = b1 - a1 + 1, b2 - a2 + 1
        print("  左右射程差 = %d px %s" % (abs(dl - dr), "✓ 对称" if abs(dl - dr) <= 4 else "✗ 不对称"))
    print("  屏幕最左列 x=0 增量=%d，最右列 x=%d 增量=%d（贴边时应该都 > %d）"
          % (colmax[0], w - 1, colmax[w - 1], THRESH))

    print()
    print("── ③ 中间留空（要求：两柱之间不该有光）──")
    mid = colmax[w // 2]
    band = max(colmax[max(0, w // 2 - 20):w // 2 + 20]) if w > 40 else colmax[w // 2]
    print("  屏幕正中 x=%d 处最大增量 = %d %s" % (w // 2, mid, "✓ 是空的" if mid <= THRESH else "✗ 中间有光"))
    print("  正中 ±20 列范围内的最大增量 = %d %s" % (band, "✓ 是空的" if band <= THRESH else "✗ 中间有光"))

    print()
    print("── ⑥ 左右镜像对称性（★ 用来抓「背景变了」这种污染）──")
    # 光柱按构造就是关于中线镜像对称的。所以**差分图也应当镜像对称**。
    # 一旦两次截图的背景不一致（实测踩过：桌面图标/文字在两张图之间重绘了，
    # 于是在差分图里留下亮斑，看着像「光柱的一部分」），这个数就会明显变大。
    # 有了它，「差分图里那块亮的是光柱还是背景变化」就不用靠眼睛判断了。
    md = [abs(colmax[x] - colmax[w - 1 - x]) for x in range(w // 2)]
    bad = [x for x, d in enumerate(md) if d > 3]
    print("  左右对应列的增量差：平均 %.1f，最大 %d，超过 3 的列数 %d/%d"
          % (sum(md) / len(md), max(md), len(bad), w // 2))
    if bad:
        print("  不对应的列（前 10 个）：%s" % bad[:10])
    print("  %s" % ("✓ 镜像对称 —— 差分里只有覆盖层画的东西" if not bad
                    else "✗ 不对称 —— 两次截图的背景不一致，本次几何结论不可信"))

    print()
    print("── ④ 增量最大的那个点（= 光柱最亮处）──")
    best = (0, 0, 0)
    for y in range(TOP, h):
        for x in range(w):
            d = delta(x, y)
            if d > best[0]:
                best = (d, x, y)
    d, x, y = best
    i = x * 3
    print("  (%d,%d) 增量=%d  off=(%d,%d,%d) → on=(%d,%d,%d)"
          % (x, y, d, A[y][i], A[y][i + 1], A[y][i + 2], B[y][i], B[y][i + 1], B[y][i + 2]))

    print()
    print("── ⑤ 采样几列的增量剖面（屏幕坐标 y，从顶往下）──")
    for x in (1, 8, 24, 48, 80, w // 2):
        prof = [delta(x, yy) for yy in range(h)]
        lit = [yy for yy in range(TOP, h) if prof[yy] > THRESH]
        if lit:
            print("  x=%-4d 发亮行 y=%d..%d（%d 行），该列最大增量 %d"
                  % (x, lit[0], lit[-1], len(lit), max(prof)))
        else:
            print("  x=%-4d 无光" % x)


if __name__ == "__main__":
    main()
