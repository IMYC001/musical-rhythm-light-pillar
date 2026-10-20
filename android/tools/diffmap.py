"""把两张截图做差分，输出热力图 + 关键剖面，用于定位「光柱到底画在哪」。"""
import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
from pngtool import read, write, rgbrows

a_p, b_p, out_p = sys.argv[1], sys.argv[2], sys.argv[3]
w, h, ch, A = read(a_p)
w2, h2, ch2, B = read(b_p)
A = rgbrows(A, ch); B = rgbrows(B, ch2)

rows = []
for y in range(h):
    ra, rb = A[y], B[y]
    line = bytearray()
    for x in range(w):
        d = (abs(ra[x*3]-rb[x*3]) + abs(ra[x*3+1]-rb[x*3+1]) + abs(ra[x*3+2]-rb[x*3+2])) // 3
        v = min(255, d * 8)
        line += bytes([v, v, v])
    rows.append(bytes(line))
write(out_p, w, h, 3, rows)
print('wrote', out_p)

# 每 32 列的最大差值 → 看光柱横向落在哪
print('\n列块最大差值 (每32列):')
for x0 in range(0, w, 32):
    mx = 0
    for y in range(0, h, 2):
        ra, rb = A[y], B[y]
        for x in range(x0, min(x0+32, w), 2):
            d = (abs(ra[x*3]-rb[x*3]) + abs(ra[x*3+1]-rb[x*3+1]) + abs(ra[x*3+2]-rb[x*3+2])) // 3
            if d > mx: mx = d
    print('  x %3d-%3d : %d' % (x0, x0+31, mx))
