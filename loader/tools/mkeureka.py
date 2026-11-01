#!/usr/bin/env python3
"""Generate the source-drawn rainbow apple/Eureka raster as nonblack row runs.

Uses Pillow's bundled Aileron font; no downloaded artwork or fonts. The preview
and target executable share the exact same raster and six rainbow colors.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

W, H, SS = 750, 1334, 3
out = Path(__file__).resolve().parents[1] / 'out'
out.mkdir(exist_ok=True)
mask = Image.new('L', (W * SS, H * SS))
draw = ImageDraw.Draw(mask)

def path(start, curves):
    points = [start]
    current = start
    for a, b, end in curves:
        for i in range(1, 41):
            t = i / 40
            u = 1 - t
            points.append(tuple(u**3 * current[k] + 3*u*u*t*a[k] +
                                3*u*t*t*b[k] + t**3*end[k] for k in (0, 1)))
        current = end
    draw.polygon([((x - 245) * 1.18 * SS + 375*SS,
                   (y - 57) * 1.18 * SS + 310*SS) for x, y in points], fill=255)

# Original Bezier construction: lobed fruit, right-hand bite, detached leaf.
path((255, 180), [
    ((220,180), (207,158), (168,164)),
    ((100,170), (66,225), (68,290)),
    ((70,367), (113,450), (152,468)),
    ((182,483), (213,460), (244,461)),
    ((275,461), (297,483), (331,469)),
    ((371,451), (406,390), (423,347)),
    ((361,323), (350,248), (412,209)),
    ((384,171), (351,162), (324,165)),
    ((297,168), (271,181), (255,180)),
])
path((254,155), [
    ((252,105), (290,61), (337,57)),
    ((341,107), (307,150), (254,155)),
])
mask = mask.resize((W, H), Image.Resampling.LANCZOS)
# Reuse the source-drawn silhouette for the loader's centered white boot logo.
# Only row spans are linked into the freestanding loader; no runtime decoding.
fruit = mask.crop(mask.getbbox())
fruit.thumbnail((150, 180), Image.Resampling.LANCZOS)
boot = Image.new('RGB', (W, H))
bx, by = (W-fruit.width)//2, (H-fruit.height)//2
runs = []
for y in range(fruit.height):
    x = 0
    while x < fruit.width:
        if fruit.getpixel((x,y)) < 128:
            x += 1
            continue
        end = x+1
        while end < fruit.width and fruit.getpixel((end,y)) >= 128:
            end += 1
        runs.extend((by+y, bx+x, bx+end))
        ImageDraw.Draw(boot).line((bx+x,by+y,bx+end-1,by+y), fill='white')
        x = end
(out / 'boot-logo.h').write_text(
    '/* Generated white logo: y,x,end row spans. */\n'
    'static const unsigned short boot_logo[]={'+','.join(map(str,runs))+'};\n')
boot.save(out / 'boot-preview.png')
colors = [0x61bb46, 0xfdb827, 0xf5821f, 0xe03a3e, 0x963d97, 0x009ddc]
image = Image.new('RGB', (W, H))
pixels = image.load()
for y in range(H):
    # Leaf and top of fruit share green; equal-height stripes across fruit.
    color = colors[max(0, min(5, (y - 433) * 6 // 375))]
    rgb = (color >> 16, (color >> 8) & 255, color & 255)
    for x in range(W):
        a = mask.getpixel((x, y))
        if a:
            pixels[x,y] = tuple((v*a+127)//255 for v in rgb)
font = ImageFont.load_default(size=84*SS)
text = Image.new('L', (W*SS, 150*SS))
ImageDraw.Draw(text).text((W*SS//2, 0), 'Eureka', font=font, anchor='mt', fill=255)
text = text.resize((W, 150), Image.Resampling.LANCZOS)
image.paste((242,242,245), (0, 910, W, 1060), text)
image.save(out / 'eureka-preview.png')
# Threshold coverage to keep the complete static executable within one 16K
# text segment. The same crisp pixel edges are used in the host preview.
encoded = []
for y in range(H):
    color = colors[max(0, min(5, (y - 433) * 6 // 375))] if y < 910 else 0xf2f2f5
    rgb = (color >> 16, (color >> 8) & 255, color & 255)
    x = 0
    while x < W:
        c = pixels[x,y]
        on = max(c) >= 110
        end = x + 1
        while end < W and (max(pixels[end,y]) >= 110) == on:
            end += 1
        for xx in range(x,end):
            pixels[xx,y] = rgb if on else (0,0,0)
        if on:
            encoded.extend((x,end))
        x = end
    encoded.append(65535)
image.save(out / 'eureka-preview.png')
header = ('/* Generated source-drawn art: pairs x,end; 65535 ends each row. */\n'
          'static const unsigned short art[]={\n')
header += ','.join(str(v) for v in encoded) + '};\n'
(out / 'eureka-art.h').write_text(header)
print(f'Eureka: {len(encoded)*2} art bytes, {W}x{H}; out/eureka-preview.png')
