#!/usr/bin/env python3
"""逐帧比对两条分析链的输出，给出可证伪的数字。

用法:
  python tools/specdiff.py <原生.csv> <原作品.csv> [--quiet]

两份 CSV 必须行数相同、列顺序相同（都由 audiotest.exe / jsref.js 生成）。
输出：
  · 每个量的最大/平均绝对差、以及相对幅度（平均差 / 原作品的峰值）
  · **包络峰值比** —— 用户报「幅度没有 Electron 大」时，看的正是这个数：
      peak(原生) / peak(原作品)。1.000 = 幅度一致。
  · 判定：所有量 maxAbs < 2e-4 视为「逐帧一致」。

退出码：0 = 一致，2 = 有超差。
"""
import os
import sys

TOL = 2e-4


def load(p):
    with open(p, encoding="utf-8") as f:
        lines = [l.rstrip("\n").rstrip("\r") for l in f if l.strip()]
    head = lines[0].split(",")
    rows = []
    for l in lines[1:]:
        parts = l.split(",")
        rows.append([float(x) for x in parts[1:]])
    return head[1:], rows


def peaks_mode(paths):
    """把多份输出并排比「每个量的峰值」——用来量化「改分析频率值多少」。

    为什么看峰值而不是逐帧差：分析频率一变，帧数就不一样了，逐帧对不齐。
    而用户说的「响应幅度大不大」，本质上就是**包络峰值**。
    """
    cols = []
    for p in paths:
        h, r = load(p)
        cols.append((os.path.basename(p), h, [max(x[i] for x in r) for i in range(len(h))]))
    names = cols[0][1]
    base = cols[-1][2]          # 以最后一份（桌面版基准）为分母
    print("%-18s" % "量" + "".join("%16s" % c[0][:15] for c in cols) + "%12s" % "末列/首列")
    print("-" * (18 + 16 * len(cols) + 12))
    for i, n in enumerate(names):
        row = "%-18s" % n + "".join("%16.6f" % c[2][i] for c in cols)
        first = cols[0][2][i]
        row += "%12.4f" % (base[i] / first if first > 1e-12 else float("nan"))
        print(row)
    print("-" * (18 + 16 * len(cols) + 12))
    print("（末列/首列 < 1 表示后面的配置幅度更小）")
    return 0


def main():
    if "--peaks" in sys.argv:
        args = [a for a in sys.argv[1:] if not a.startswith("--")]
        if len(args) < 2:
            print("用法: python tools/specdiff.py --peaks <a.csv> <b.csv> [...]")
            return 1
        return peaks_mode(args)
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    quiet = "--quiet" in sys.argv
    ha, ra = load(sys.argv[1])
    hb, rb = load(sys.argv[2])
    if ha != hb:
        print("!! 表头不一致：%s vs %s" % (ha, hb))
        return 1
    if len(ra) != len(rb):
        print("!! 行数不一致：%d vs %d" % (len(ra), len(rb)))
        return 1

    n = len(ha)
    peak_a = [max(r[i] for r in ra) for i in range(n)]
    peak_b = [max(r[i] for r in rb) for i in range(n)]

    worst = 0.0
    if not quiet:
        print("%-18s %12s %12s %12s %10s" %
              ("量", "原生峰值", "原作品峰值", "峰值比", "最大绝对差"))
        print("-" * 70)
    for i in range(n):
        m = 0.0
        for k in range(len(ra)):
            d = abs(ra[k][i] - rb[k][i])
            if d > m:
                m = d
        if m > worst:
            worst = m
        if not quiet:
            ratio = (peak_a[i] / peak_b[i]) if peak_b[i] > 1e-12 else float("nan")
            print("%-18s %12.6f %12.6f %12.6f %12.3e" %
                  (ha[i], peak_a[i], peak_b[i], ratio, m))
    print("-" * 70)
    print("帧数 %d　最大绝对差 %.3e　容差 %.0e" % (len(ra), worst, TOL))
    # 幅度比 —— 用户那句「响应幅度没有 Electron 大」的量化判据
    keys = ["bass", "subBass", "lowMid", "energy"]
    for k in keys:
        if k in ha:
            i = ha.index(k)
            if peak_b[i] > 1e-12:
                print("  %-10s 峰值比 原生/原作品 = %.4f" % (k, peak_a[i] / peak_b[i]))
    ok = worst < TOL
    print("判定: %s" % ("逐帧一致 ✓" if ok else "存在偏差 ✗"))
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
