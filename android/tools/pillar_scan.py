"""在「有覆盖层 / 无覆盖层」两张同页截图之间，逐行找左右光柱的贡献。
用来回答：光柱画在屏幕哪一段？合成后贡献了多少？"""
import sys
sys.path.insert(0, 'tools')
from pngtool import read, rgbrows

off_p, on_p = sys.argv[1], sys.argv[2]
w, h, ch, A = read(off_p)
_, _, ch2, B = read(on_p)
A = rgbrows(A, ch); B = rgbrows(B, ch2)

print('y     左边缘(off->on)      右边缘(off->on)      行内最大增量')
rows_out = []
for y in range(h):
    ra, rb = A[y], B[y]
    def band(x0, x1):
        o = max(sum(ra[x*3:x*3+3]) / 3.0 for x in range(x0, x1))
        n = max(sum(rb[x*3:x*3+3]) / 3.0 for x in range(x0, x1))
        return o, n
    lo, ln = band(0, 6)
    ro, rn = band(w - 6, w)
    gain = 0
    for x in range(w):
        d = (sum(rb[x*3:x*3+3]) - sum(ra[x*3:x*3+3])) / 3.0
        if d > gain: gain = d
    if gain > 1.5:
        rows_out.append((y, lo, ln, ro, rn, gain))

print('共 %d 行有增量(>1.5)' % len(rows_out))
for y, lo, ln, ro, rn, g in rows_out:
    print('  y=%3d  左 %5.1f -> %5.1f (+%4.1f)   右 %5.1f -> %5.1f (+%4.1f)   行内最大 +%.1f'
          % (y, lo, ln, ln - lo, ro, rn, rn - ro, g))

# 找全图最大增量像素
best = (0, 0, 0)
for y in range(h):
    ra, rb = A[y], B[y]
    for x in range(w):
        d = (sum(rb[x*3:x*3+3]) - sum(ra[x*3:x*3+3])) / 3.0
        if d > best[0]:
            best = (d, x, y)
print('\n全图最大增量: +%.1f @ (x=%d, y=%d)' % best)
d, x, y = best
print('  该点 off RGB =', tuple(A[y][x*3:x*3+3]), ' on RGB =', tuple(B[y][x*3:x*3+3]))
