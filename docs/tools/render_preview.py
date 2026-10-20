#!/usr/bin/env python3
"""按着色器数学渲染预览图（不需要 GPU / 不需要跑程序）。

为什么要有这个东西：README 需要几张干净的效果图，而实拍截图受桌面壁纸、
图标、分辨率影响，还会把用户桌面拍进去。这里把 src/shader_src.h 里的
像素着色器逐行翻成 numpy，参数一给就能出图 —— 结果和程序渲染的一致
（同一套公式、同一套主题色），只是驱动量是手工指定的常量。

    python docs/tools/render_preview.py hero   docs/images/hero.png
    python docs/tools/render_preview.py themes docs/images/themes.png
    python docs/tools/render_preview.py styles docs/images/styles.png

依赖：numpy。PNG 读写是纯标准库。
"""
import math
import os
import struct
import sys
import zlib

import numpy as np


def load_font(size):
    """找一个能显示中文的字体。找不到就退化到 PIL 默认字体（中文会变方框）。"""
    for p in (
        "C:/Windows/Fonts/msyh.ttc",          # Windows 微软雅黑
        "C:/Windows/Fonts/simhei.ttf",
        "/System/Library/Fonts/PingFang.ttc",  # macOS
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    ):
        if os.path.exists(p):
            try:
                from PIL import ImageFont
                return ImageFont.truetype(p, size)
            except OSError:
                continue
    from PIL import ImageFont
    return ImageFont.load_default(size)

# ============================================================
#  主题表 —— 与 src/data_themes.h / android 的 Themes.kt 逐字一致
#  顺序：(key, 中文名, cool, warm, peak, glow)
# ============================================================
THEMES = [
    ("nocturnal", "霁紫", (0.350, 0.100, 0.900), (0.650, 0.250, 1.000), (1.000, 0.550, 0.050), 1.0000),
    ("ocean-deep", "沧蓝", (0.000, 0.250, 1.000), (0.150, 0.550, 1.000), (1.000, 0.750, 0.100), 1.1000),
    ("arctic-aurora", "冰蓝", (0.000, 0.750, 0.850), (0.200, 1.000, 0.850), (1.000, 0.250, 0.350), 1.2500),
    ("cyber-forest", "碧翠", (0.000, 0.850, 0.350), (0.400, 1.000, 0.300), (1.000, 0.200, 0.500), 1.3000),
    ("golden-hour", "流金", (0.850, 0.600, 0.050), (1.000, 0.920, 0.350), (0.200, 0.500, 1.000), 1.2000),
    ("ember-fire", "余烬", (1.000, 0.450, 0.000), (1.000, 0.780, 0.150), (0.100, 0.400, 1.000), 1.5000),
    ("crimson-sunset", "赤焰", (1.000, 0.050, 0.080), (1.000, 0.350, 0.200), (0.100, 0.900, 0.700), 1.4000),
    ("coral-mirage", "霞粉", (1.000, 0.250, 0.300), (1.000, 0.550, 0.550), (0.100, 0.700, 1.000), 1.3000),
    ("neon-tokyo", "幻紫", (1.000, 0.050, 0.600), (1.000, 0.250, 0.850), (0.950, 1.000, 0.150), 1.6000),
    ("minimal-monochrome", "水墨", (0.800, 0.800, 0.800), (1.000, 1.000, 1.000), (1.000, 1.000, 1.000), 0.7000),
    ("teal-depth", "幽青", (0.000, 0.550, 0.550), (0.200, 0.850, 0.750), (1.000, 0.450, 0.150), 1.2000),
    ("lavender-dream", "薰衣草", (0.550, 0.350, 0.850), (0.750, 0.550, 1.000), (1.000, 0.800, 0.250), 1.1000),
    ("cherry-blossom", "樱", (1.000, 0.550, 0.650), (1.000, 0.720, 0.780), (0.250, 0.900, 0.550), 1.1500),
    ("copper-forge", "锻铜", (0.850, 0.450, 0.200), (1.000, 0.650, 0.300), (0.300, 0.650, 0.350), 1.3000),
    ("mint-fresh", "薄荷", (0.300, 0.900, 0.650), (0.500, 1.000, 0.800), (1.000, 0.300, 0.550), 1.2000),
]


# ============================================================
#  PNG 读写（纯标准库，不依赖 Pillow）
# ============================================================
def write_png(path, rgb):
    """rgb: HxWx3 uint8"""
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


# ============================================================
#  着色器数学
# ============================================================
def lin2srgb(c):
    c = np.clip(c, 0.0, 1.0)
    lo = c * 12.92
    hi = 1.055 * np.power(np.maximum(c, 1e-8), 1.0 / 2.4) - 0.055
    return np.where(c <= 0.0031308, lo, hi)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lerp(a, b, t):
    return a + (b - a) * t


def render(w, h, theme, *, drive=0.62, style=0, inset=0.0, alpha_k=0.0,
           idle=0.0, time=0.0, peak_on=1.0, peak_int=0.35, intensity=1.0,
           response=1.0, bg=None):
    """返回 HxWx3 float（已合成背景）与 HxW 的 alpha。"""
    _, _name, cool, warm, peak, glow = theme
    cCool = np.array(lin2srgb(np.array(cool)))
    cWarm = np.array(lin2srgb(np.array(warm)))
    cPk = np.array(lin2srgb(np.array(peak)))

    ys, xs = np.mgrid[0:h, 0:w]
    u = (xs + 0.5) / w
    v = 1.0 - (ys + 0.5) / h          # HLSL: uv.y = 0 在下边
    asp = w / h

    low = 0.0 * 1.20 + 0.0 * 0.85 + 0.0 * 0.25
    d = math.fmod(drive, 1.70) if drive > 1.70 else drive
    d = max(0.0, min(1.70, drive))

    dn = max(0.0, min(1.0, d / 1.30))
    if style < 0.5:
        hh = d * 0.62
        head2 = smoothstep(0.50, 0.95, dn)
        amp = (0.50 + d * 0.85) * glow
        amp *= smoothstep(0.0, 0.16, d + idle)
        c = lerp(cCool, cWarm, max(0.0, min(1.0, d * 0.55)))
        pk = max(0.0, min(1.0, dn * dn * peak_on * peak_int))
        c = lerp(c, cPk, pk * 0.45)
    else:
        hh = 1.0
        head2 = 0.0
        flash = smoothstep(0.18, 0.95, d)
        cFlash = lerp(cWarm, cPk, max(0.0, min(1.0, peak_on * peak_int)))
        c = lerp(cCool, cFlash, flash)
        amp = 0.85 * glow * (1.0 + flash * 0.85) * lerp(1.0, 0.50, idle)

    breathe = 0.66 + 0.20 * math.sin(time * 0.55) + 0.14 * math.sin(time * 0.23 + 1.7)
    hh += max(0.0, breathe) * idle * 0.48

    # 安卓版加的纵横比补偿：让「inset 拉到最低」在任何纵横比下都离场
    WX = 0.26 * min(1.0, asp / 1.778)
    if abs(inset) > 0.001:
        WX = min(WX, abs(inset) * asp * 0.9)
    EY = lerp(0.012, 0.14, head2)

    col = np.zeros((h, w, 3))
    for i in (0, 1):
        e0 = (u if i == 0 else 1.0 - u) - inset
        dx = e0 * asp
        dy = np.maximum(0.0, v - hh)
        dist = np.sqrt(dx * dx + dy * dy)
        g = 1.0 - smoothstep(0.0, WX, dist)
        ay = 1.0 - smoothstep(0.0, EY, v - hh)

        halo = 0.0
        if style < 0.5:
            rx = dx / WX
            ry = (v - hh) / (WX * 1.25)
            rr = np.sqrt(rx * rx + ry * ry)
            halo = np.power(np.maximum(1.0 - rr, 0.0), 2.2) * head2

        grad = lerp(0.55, 1.0, np.clip(v / max(hh, 0.001), 0.0, 1.0))
        wgt = (g * ay * grad + halo * 0.62) * amp
        col += c[None, None, :] * wgt[:, :, None]

    col = col / (1.0 + col * 0.5)

    a = np.max(col, axis=2) * alpha_k + 0.004

    if bg is None:
        bg = np.zeros((h, w, 3))
    out = col + bg * (1.0 - a[:, :, None])
    return out, a


# ============================================================
#  背景：一块很暗的抽象渐变（模拟桌面，而不是纯黑）
#  故意做得几乎看不见 —— 光柱是加光，底色越暗越接近真实观感
# ============================================================
def make_bg(w, h, seed=0, tint=(0.055, 0.045, 0.085)):
    ys, xs = np.mgrid[0:h, 0:w]
    u = xs / w
    v = 1.0 - ys / h
    base = np.zeros((h, w, 3))
    for k in range(3):
        base[:, :, k] = tint[k]
    # 左上到右下的柔和渐变 + 一点低频起伏
    base *= (0.35 + 1.25 * np.power(v * 0.7 + (1.0 - u) * 0.3, 1.4))[:, :, None]
    wob = 0.06 * np.sin(u * 5.1 + seed) * np.sin(v * 3.7 + seed * 1.7)
    base += wob[:, :, None] * 0.35
    return np.clip(base, 0.0, 1.0)


def to8(x):
    return np.clip(x * 255.0 + 0.5, 0, 255).astype(np.uint8)


# ============================================================
#  三张图
# ============================================================
def make_hero(path, w=1920, h=1080):
    th = THEMES[0]  # 霁紫
    # 背景比正文里的示意图再亮一点点：README 顶部那张会被缩到 ~830px 宽，
    # 纯黑底 + 细光柱缩完之后只剩两条淡影子。抬一点底色，缩略时才看得出
    # 「光打在桌面上」而不是「两条紫线」。
    bg = make_bg(w, h, seed=0.7, tint=(0.050, 0.044, 0.082))
    out, _ = render(w, h, th, drive=1.00, style=0, inset=0.0, alpha_k=0.0,
                    idle=0.14, time=1.2, peak_on=1.0, peak_int=0.45, bg=bg)
    write_png(path, to8(out))
    print("hero ->", path, w, h)


def make_themes(path, cols=5, cw=376, ch=212, gap=18):
    from PIL import Image, ImageDraw, ImageFont

    rows = (len(THEMES) + cols - 1) // cols
    W = cols * cw + (cols - 1) * gap
    H = rows * ch + (rows - 1) * gap
    canvas = Image.new("RGB", (W, H), (8, 8, 12))

    # 驱动量各不相同：一排里三个安静、一个高潮，看起来像真在跟着音乐动
    drives = [0.42, 0.58, 0.33, 0.80, 0.50, 0.66, 0.38, 0.74,
              0.46, 0.62, 0.35, 0.70, 0.55, 0.44, 0.68, 0.40]

    font = load_font(17)
    font_s = load_font(13)

    for idx, th in enumerate(THEMES):
        r, c = divmod(idx, cols)
        y0 = r * (ch + gap)
        x0 = c * (cw + gap)
        bg = make_bg(cw, ch, seed=idx * 0.9, tint=(0.020, 0.018, 0.032))
        out, _ = render(cw, ch, th, drive=drives[idx % len(drives)], style=0,
                        idle=0.25, time=idx * 0.7, peak_int=0.4)
        tile = Image.fromarray(to8(np.clip(out + bg * 0.55, 0, 1)))
        d = ImageDraw.Draw(tile, "RGBA")
        # 1px 描边：不然相邻格子的右柱和左柱会挨在一起，看着像「每格只有一根」
        d.rectangle([0, 0, cw - 1, ch - 1], outline=(60, 62, 78, 255))
        # 文字放**正中**：两柱之间是空的，放角落会被光柱压住
        d.text((cw // 2, ch - 36), th[1], font=font, fill=(240, 240, 250, 240), anchor="mb")
        d.text((cw // 2, ch - 14), th[0], font=font_s, fill=(150, 152, 172, 200), anchor="mb")
        canvas.paste(tile, (x0, y0))

    canvas.save(path)
    print("themes ->", path, W, H, "(%d 套)" % len(THEMES))


def make_styles(path, cw=960, ch=540, gap=16):
    """上：风格①（光柱跳动）；下：风格②（常驻光柱 + 节拍闪色）

    两块都用 16:9。之前试过「左右各半、各 940x1040」—— 那样每块纵横比 0.9，
    光柱横向射程按画面高归一化，视觉上柱子会被拉得又宽又糊，和真实观感差很远。
    """
    from PIL import Image, ImageDraw, ImageFont

    W = cw
    H = ch * 2 + gap
    canvas = Image.new("RGB", (W, H), (8, 8, 12))
    th = THEMES[1]  # 沧蓝
    font = load_font(20)
    font_s = load_font(15)

    labels = [("风格 ①　光柱跳动", "安静时几乎看不见，越响越高"),
              ("风格 ②　常驻光柱 · 节拍闪色", "柱子常在，响的时候整根换色")]
    for i, style in enumerate((0, 1)):
        bg = make_bg(cw, ch, seed=3 + i, tint=(0.018, 0.020, 0.040))
        out, _ = render(cw, ch, th, drive=0.62 if style == 0 else 0.34, style=style,
                        idle=0.30, time=2.0, peak_int=0.5)
        tile = Image.fromarray(to8(np.clip(out + bg * 0.5, 0, 1)))
        d = ImageDraw.Draw(tile, "RGBA")
        d.text((24, 22), labels[i][0], font=font, fill=(240, 240, 250, 240))
        d.text((24, 50), labels[i][1], font=font_s, fill=(158, 162, 184, 205))
        canvas.paste(tile, (0, i * (ch + gap)))
    canvas.save(path)
    print("styles ->", path, W, H)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    what = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    if what == "hero":
        make_hero(out or "hero.png")
    elif what == "themes":
        make_themes(out or "themes.png")
    elif what == "styles":
        make_styles(out or "styles.png")
    else:
        print("未知目标:", what)


if __name__ == "__main__":
    main()
