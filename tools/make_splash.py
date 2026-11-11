#!/usr/bin/env python3
"""Pravi OIR boot splash: blue_mist pozadina + 'i<heart>Radio' (srce crveno).

Srce se crta rucno (parametarska kriva), jer sistemski Noto font nema U+2665
(prikazuje .notdef kvadrat).
"""
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H = 800, 480
SRC = "/home/fox/Pictures/blue_mist.avif"
OUT_PNG = "/tmp/splash.png"
OUT_565 = "/tmp/splash800.rgb565"
FONT = "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf"
TEXT_PX = 132
HEART = (229, 57, 53)
WHITE = (240, 240, 245)


def load_bg():
    bg = Image.open("/tmp/bm.png").convert("RGB")
    s = max(W / bg.width, H / bg.height)
    bg = bg.resize((int(bg.width * s + 0.5), int(bg.height * s + 0.5)), Image.LANCZOS)
    left = (bg.width - W) // 2
    top = (bg.height - H) // 2
    bg = bg.crop((left, top, left + W, top + H))
    return Image.blend(bg, Image.new("RGB", (W, H), (0, 0, 0)), 0.35)


def heart_image(width_px):
    """Vrati RGBA sliku srca sirine width_px (antialiased)."""
    import math
    SS = 4
    pts = []
    for i in range(361):
        t = math.radians(i)
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((x, y))
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    w = max(xs) - min(xs); h = max(ys) - min(ys)
    scale = (width_px * SS) / w
    img = Image.new("RGBA", (int(w * scale) + 2, int(h * scale) + 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    poly = [((p[0] - min(xs)) * scale + 1, (max(ys) - p[1]) * scale + 1) for p in pts]
    d.polygon(poly, fill=HEART + (255,))
    return img.resize((img.width // SS, img.height // SS), Image.LANCZOS)


def main():
    bg = load_bg()
    f = ImageFont.truetype(FONT, TEXT_PX)
    try:
        f.set_variation_by_name("Bold")
    except Exception:
        pass
    asc, desc = f.getmetrics()
    cap_h = int(asc * 0.74)                 # priblizna visina velikih slova
    heart = heart_image(int(cap_h * 1.15))
    heart = heart.resize((heart.width, cap_h), Image.LANCZOS)

    w_i = f.getlength("i")
    w_r = f.getlength("Radio")
    gap = 10
    total = w_i + gap + heart.width + gap + w_r
    x = (W - total) / 2
    y_text = H / 2 - (asc + desc) / 2
    # srce vertikalno centrirano na vizuelnu sredinu teksta (ne na vrh kape)
    y_heart = y_text + asc - cap_h + 3   # vizuelno poravnanje sa sredinom teksta

    # senka
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ds = ImageDraw.Draw(sh)
    xx = x
    ds.text((xx + 3, y_text + 4), "i", font=f, fill=(0, 0, 0, 170))
    xx += w_i + gap
    sh.paste(heart, (int(xx) + 3, int(y_heart) + 4), heart)
    xx += heart.width + gap
    ds.text((xx + 3, y_text + 4), "Radio", font=f, fill=(0, 0, 0, 170))
    sh = sh.filter(ImageFilter.GaussianBlur(4))
    img = Image.alpha_composite(bg.convert("RGBA"), sh)

    d = ImageDraw.Draw(img)
    xx = x
    d.text((xx, y_text), "i", font=f, fill=WHITE)
    xx += w_i + gap
    img.paste(heart, (int(xx), int(y_heart)), heart)
    xx += heart.width + gap
    d.text((xx, y_text), "Radio", font=f, fill=WHITE)

    img.convert("RGB").save(OUT_PNG)
    print(f"splash: {OUT_PNG}  text width {int(total)}  heart {heart.width}x{heart.height}")


if __name__ == "__main__":
    main()
