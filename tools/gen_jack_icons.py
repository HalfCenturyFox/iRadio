#!/usr/bin/env python3
"""Generise LVGL ARGB8888 ikonice audio jack-a iz PNG-a (bela pozadina).

Upotreba: gen_jack_icons.py ulaz.png izlaz.h [velicine...]
Podrazumevano: 24, 48 px.
"""
import sys
from PIL import Image

src = sys.argv[1]
out = sys.argv[2]
sizes = [int(s) for s in sys.argv[3:]] or [24, 48]

im = Image.open(src).convert("L")
w, h = im.size
px = im.load()
dark = [(x, y) for y in range(h) for x in range(w) if px[x, y] < 128]
xs = [p[0] for p in dark]
ys = [p[1] for p in dark]
box = (max(0, min(xs) - 8), max(0, min(ys) - 8),
       min(w, max(xs) + 8), min(h, max(ys) + 8))
im = im.crop(box)
side = max(im.size)
sq = Image.new("L", (side, side), 255)
sq.paste(im, ((side - im.size[0]) // 2, (side - im.size[1]) // 2))

lines = ["/* Auto-generisano: gen_jack_icons.py — ne menjati rucno. */",
         "#ifndef OIR_JACK_ICONS_H", "#define OIR_JACK_ICONS_H",
         "#include <lvgl.h>", ""]
for sz in sizes:
    ic = sq.resize((sz, sz), Image.LANCZOS)
    data = []
    for y in range(sz):
        for x in range(sz):
            a = 255 - ic.getpixel((x, y))     # tamne linije -> neprozirno
            data += [255, 255, 255, a]        # B,G,R,A (LVGL redosled), belo za recolor
    name = f"icon_jack_{sz}"
    lines.append(f"static const uint8_t {name}_map[] = {{")
    for i in range(0, len(data), 16):
        lines.append("    " + ",".join(str(v) for v in data[i:i + 16]) + ",")
    lines.append("};")
    lines.append(f"static const lv_image_dsc_t {name} = {{")
    lines.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("    .header.cf = LV_COLOR_FORMAT_ARGB8888,")
    lines.append(f"    .header.w = {sz},")
    lines.append(f"    .header.h = {sz},")
    lines.append(f"    .data_size = {sz * sz * 4},")
    lines.append(f"    .data = {name}_map,")
    lines.append("};")
    lines.append("")
lines.append("#endif")
open(out, "w").write("\n".join(lines) + "\n")
print(f"{out}: {', '.join(str(s) for s in sizes)} px")
