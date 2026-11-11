from PIL import Image, ImageDraw
import os

SZ = 48
SS = 6  # supersample
def canvas():
    return Image.new('RGBA', (SZ*SS, SZ*SS), (255,255,255,0))

def draw_play():
    im = canvas(); d = ImageDraw.Draw(im)
    S = SZ*SS
    d.polygon([(S*0.30, S*0.18), (S*0.30, S*0.82), (S*0.80, S*0.50)], fill=(255,255,255,255))
    return im

def draw_pause():
    im = canvas(); d = ImageDraw.Draw(im); S = SZ*SS
    w = S*0.14
    d.rectangle([S*0.31, S*0.20, S*0.31+w, S*0.80], fill=(255,255,255,255))
    d.rectangle([S*0.55, S*0.20, S*0.55+w, S*0.80], fill=(255,255,255,255))
    return im

def draw_next():
    im = canvas(); d = ImageDraw.Draw(im); S = SZ*SS
    d.polygon([(S*0.24, S*0.18), (S*0.24, S*0.82), (S*0.66, S*0.50)], fill=(255,255,255,255))
    d.rectangle([S*0.70, S*0.20, S*0.78, S*0.80], fill=(255,255,255,255))
    return im

def draw_prev():
    im = canvas(); d = ImageDraw.Draw(im); S = SZ*SS
    d.rectangle([S*0.22, S*0.20, S*0.30, S*0.80], fill=(255,255,255,255))
    d.polygon([(S*0.76, S*0.18), (S*0.76, S*0.82), (S*0.34, S*0.50)], fill=(255,255,255,255))
    return im

icons = {'play': draw_play, 'pause': draw_pause, 'next': draw_next, 'prev': draw_prev}
out = []
os.makedirs('/home/fox/Work/icons', exist_ok=True)
out.append('#ifndef OIR_ICONS_H\n#define OIR_ICONS_H\n\n#include <lvgl.h>\n')
for name, fn in icons.items():
    im = fn().resize((SZ, SZ), Image.LANCZOS)
    im.save(f'/home/fox/Work/icons/{name}.png')
    px = im.load()
    data = []
    for y in range(SZ):
        for x in range(SZ):
            r,g,b,a = px[x,y]
            data += [b,g,r,a]   # LVGL ARGB8888 = B,G,R,A in memory
    out.append(f'static const uint8_t icon_{name}_map[] = {{\n')
    for i in range(0, len(data), 24):
        out.append('    ' + ','.join(str(v) for v in data[i:i+24]) + ',\n')
    out.append('};\n\n')
    out.append(f'''static const lv_image_dsc_t icon_{name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.w = {SZ},
    .header.h = {SZ},
    .data_size = {SZ*SZ*4},
    .data = icon_{name}_map,
}};

''')
out.append('#endif\n')
open('/home/fox/Work/icons.h','w').write(''.join(out))
print('icons.h bytes', os.path.getsize('/home/fox/Work/icons.h'))
