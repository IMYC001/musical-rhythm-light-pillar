# BMP(32bpp BGRA) -> PNG，纯标准库（没有 PIL）。
# 面板渲染缓冲是**预乘 alpha**，直接看会很暗，所以合成到一块中灰底上，
# 这样才能看清它叠在桌面上时是什么样子。
import struct, zlib, sys


def read_bmp32(path):
    d = open(path, 'rb').read()
    assert d[:2] == b'BM', '不是 BMP'
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    assert bpp == 32, '只支持 32bpp，实际 %d' % bpp
    topdown = h < 0
    h = abs(h)
    stride = w * 4
    px = []
    for y in range(h):
        sy = y if topdown else (h - 1 - y)
        row = d[off + sy * stride: off + sy * stride + stride]
        px.append(row)
    return w, h, px


def over(bg, b, g, r, a):
    """预乘 alpha 的 over 合成：结果 = 前景 + 背景*(1-a)"""
    af = a / 255.0
    return (
        int(min(255, b + bg[0] * (1 - af))),
        int(min(255, g + bg[1] * (1 - af))),
        int(min(255, r + bg[2] * (1 - af))),
    )


def to_png(path, out, bg=(60, 62, 70)):
    w, h, rows = read_bmp32(path)
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for x in range(w):
            b, g, r, a = row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]
            nb, ng, nr = over(bg, b, g, r, a)
            raw += bytes((nr, ng, nb))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    open(out, 'wb').write(png)
    return w, h


if __name__ == '__main__':
    print(to_png(sys.argv[1], sys.argv[2]))
