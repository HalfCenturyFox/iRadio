#!/usr/bin/env python3
"""Pravi 320x320 'i<heart>Radio' placeholder cover (RGB565) za OIR UI."""
import math
from PIL import Image, ImageDraw, ImageFont

W = H = 320
SS = 4
FONT = "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf"
BG = (18, 22, 26)
WHITE = (235, 240, 245)
RED = (229, 57, 53)
OUT = "/home/fox/Work/iRadio/assets/nocover320.rgb565"


def heart(width_px):
    pts = []
    for i in range(361):
        t = math.radians(i)
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((x, y))
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    w = max(xs) - min(xs)
    h = max(ys) - min(ys)
    sc = (width_px * SS) / w
    img = Image.new("RGBA", (int(w * sc) + 2, int(h * sc) + 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.polygon([((p[0] - min(xs)) * sc + 1, (max(ys) - p[1]) * sc + 1) for p in pts], fill=RED + (255,))
    return img.resize((img.width // SS, img.height // SS), Image.LANCZOS)


def main():
    img = Image.new("RGB", (W * SS, H * SS), BG)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, 62 * SS)
    try:
        f.set_variation_by_name("Bold")
    except Exception:
        pass
    asc, desc = f.getmetrics()
    cap = int(asc * 0.74)
    h = heart(int(cap * 1.15)).resize((int(cap * 1.15), cap), Image.LANCZOS)

    w_i = f.getlength("i")
    w_r = f.getlength("Radio")
    gap = 8 * SS
    total = w_i + gap + h.width + gap + w_r
    x = (W * SS - total) / 2
    y = (H * SS - (asc + desc)) / 2
    d.text((x, y), "i", font=f, fill=WHITE)
    x += w_i + gap
    img.paste(h, (int(x), int(y + (asc - cap))), h)
    x += h.width + gap
    d.text((x, y), "Radio", font=f, fill=WHITE)

    small = img.resize((W, H), Image.LANCZOS)
    small.save("/tmp/nocover320.png")
    raw = bytearray()
    for py in range(H):
        for px_ in range(W):
            r, g, b = small.getpixel((px_, py))
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            raw += bytes((v & 0xFF, v >> 8))
    open(OUT, "wb").write(raw)
    print(f"{OUT}: {len(raw)} B, {W}x{H} RGB565")


if __name__ == "__main__":
    main()
