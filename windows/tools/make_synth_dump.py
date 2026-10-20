#!/usr/bin/env python3
"""造一份**合成**的频谱 dump，用来先验证对照台本身是对的。

为什么先来合成的：如果直接上真实音乐，一旦两条链对不上，
分不清是「分析链真有差异」还是「对照台自己写错了」。
合成信号有已知答案（鼓点在哪、衰减多快），先把台子校准好，
再上真东西。

产出格式和 光柱.exe --dumpspec 完全一致：
  #spec128 label=... hz=30 bins=128
  label,f0,...,f127

用法: python tools/make_synth_dump.py <out.csv> [秒数]
"""
import math
import os
import sys

BINS = 128


def spectrum_at(t):
    """造一个「低频鼓点 + 中频持续音 + 高频镲」的频谱，0~1。

    鼓点：每 0.5 秒一次，指数衰减 —— 这正是用户说的「节拍」。
    """
    out = []
    kick_phase = t % 0.5
    kick = math.exp(-kick_phase * 9.0)
    # 第二个鼓点略弱（间隔 0.25s）—— 专门用来抓「部分节拍不响应」那类问题
    kick2 = 0.6 * math.exp(-((t % 0.5) - 0.25 if (t % 0.5) >= 0.25 else 1.0) * 9.0)
    for i in range(BINS):
        # 0..1 的归一化频率位置
        f = i / (BINS - 1)
        # 低频（前 12 段）：鼓点主导
        low = kick * math.exp(-f * 22.0) * 0.95
        low += kick2 * math.exp(-f * 22.0) * 0.95
        # 中频（12~50 段）：持续音，缓慢起伏
        mid = 0.30 * math.exp(-abs(f - 0.28) * 14.0) * (0.75 + 0.25 * math.sin(t * 2.1))
        # 高频（50 段之后）：镲，只在鼓点上响
        hi = 0.22 * math.exp(-abs(f - 0.72) * 9.0) * (0.25 + 0.75 * kick)
        v = low + mid + hi
        out.append(min(1.0, max(0.0, v)))
    return out


def main():
    out_path = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
    hz = 30
    n = int(secs * hz)
    label = "synth"
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("#spec128 label=%s hz=30 bins=%d\n" % (label, BINS))
        for k in range(n):
            t = k / hz
            vals = spectrum_at(t)
            f.write(label + "," + ",".join("%.9f" % v for v in vals) + "\n")
    print("已写出 %s：%d 帧 / %.1f 秒（%d Hz）" % (out_path, n, secs, hz))


if __name__ == "__main__":
    main()
