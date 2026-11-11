#!/usr/bin/env python3
"""PNG/JPG -> 320x320 RGB565 (LE) za OIR logo stanice.

Usage: ./make_logo.py <slika> <slug>
  npr. ./make_logo.py ~/slike/naxi.png naxi-radio
Rezultat: <slug>.rgb565 (204800 B) -> kopirati u /opt/radio/assets/logos565/
"""
import sys, struct
from PIL import Image

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(1)

src, slug = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGB')
im.thumbnail((304, 304), Image.LANCZOS)
canvas = Image.new('RGB', (320, 320), (18, 18, 18))
canvas.paste(im, ((320 - im.width) // 2, (320 - im.height) // 2))

buf = bytearray()
px = canvas.load()
for y in range(320):
    for x in range(320):
        r, g, b = px[x, y]
        buf += struct.pack('<H', ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

out = f"{slug}.rgb565"
open(out, 'wb').write(bytes(buf))
print(f"{out}: {len(buf)} bytes")
