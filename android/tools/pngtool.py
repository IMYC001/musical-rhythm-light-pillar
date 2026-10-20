"""纯标准库 PNG 读写 + 对比。用来对安卓截图做定量取证（不装第三方库）。"""
import zlib, struct, sys

def read(p):
    d = open(p, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not png'
    i = 8; idat = b''; w = h = ct = None
    while i < len(d):
        ln = struct.unpack('>I', d[i:i+4])[0]; ty = d[i+4:i+8]
        dt = d[i+8:i+8+ln]; i += 12 + ln
        if ty == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', dt[:10])
        elif ty == b'IDAT': idat += dt
        elif ty == b'IEND': break
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    stride = w * ch
    rows = []; prev = bytearray(stride); pos = 0
    for _ in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos+stride]); pos += stride
        if f:
            for x in range(stride):
                a = line[x-ch] if x >= ch else 0
                b = prev[x]
                c = prev[x-ch] if x >= ch else 0
                if f == 1: line[x] = (line[x] + a) & 255
                elif f == 2: line[x] = (line[x] + b) & 255
                elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
                else:
                    pp = a + b - c
                    pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                    pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                    line[x] = (line[x] + pr) & 255
        rows.append(bytes(line)); prev = line
    return w, h, ch, rows

def write(p, w, h, ch, rows):
    ct = {1: 0, 3: 2, 4: 6}[ch]
    raw = b''.join(b'\x00' + r for r in rows)
    def crc(t, dd): return struct.pack('>I', zlib.crc32(t + dd) & 0xffffffff)
    o = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', w, h, 8, ct, 0, 0, 0)
    o += struct.pack('>I', 13) + b'IHDR' + ihdr + crc(b'IHDR', ihdr)
    comp = zlib.compress(raw, 9)
    o += struct.pack('>I', len(comp)) + b'IDAT' + comp + crc(b'IDAT', comp)
    o += struct.pack('>I', 0) + b'IEND' + crc(b'IEND', b'')
    open(p, 'wb').write(o)

def px(rows, ch, x, y):
    r = rows[y]; i = x * ch
    return (r[i], r[i+1], r[i+2]) if ch >= 3 else (r[i],) * 3

def rgbrows(rows, ch):
    """统一成 3 通道。"""
    if ch == 3: return rows
    out = []
    for r in rows:
        n = len(r) // ch; b = bytearray()
        for x in range(n):
            b += bytes(r[x*ch:x*ch+3]) if ch >= 3 else bytes([r[x*ch]]*3)
        out.append(bytes(b))
    return out

def col_profile(rows3, w, h, x):
    """某列的亮度剖面。"""
    return [sum(rows3[y][x*3:x*3+3]) / 3.0 for y in range(h)]

def row_profile(rows3, w, h, y):
    return [sum(rows3[y][x*3:x*3+3]) / 3.0 for x in range(w)]

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'info':
        w, h, ch, rows = read(sys.argv[2])
        print('size', w, h, 'ch', ch)
    elif cmd == 'diff':
        w, h, ch, a = read(sys.argv[2])
        w2, h2, ch2, b = read(sys.argv[3])
        assert (w, h) == (w2, h2)
        a3 = rgbrows(a, ch); b3 = rgbrows(b, ch2)
        tot = 0; mx = 0; nz = 0
        band = [0] * 20
        for y in range(h):
            ra, rb = a3[y], b3[y]
            for x in range(w):
                d = abs(ra[x*3]-rb[x*3]) + abs(ra[x*3+1]-rb[x*3+1]) + abs(ra[x*3+2]-rb[x*3+2])
                tot += d
                if d > mx: mx = d
                if d > 6:
                    nz += 1
                    band[min(19, x * 20 // w)] += 1
        print('mean|d|=%.3f  max|d|=%d  pixels>6=%d (%.2f%%)' % (tot/(w*h*3), mx, nz, 100.0*nz/(w*h)))
        print('x-band(20) 非零像素:', band)
