#!/usr/bin/env python3
"""Font templates for hand-drawing the game's fonts.

Reads the five bitmap fonts from your game (GuiPackages Base/Game .rui for the
glyph tables, .ruirr for the images) and writes, per font, into --out:

  <Font>_1x_original.png      the game's own font image
  <Font>_4x_guide.png         transparent overlay at 4x: every glyph's box,
                              its character and code, the baseline
  <Font>_4x_preview.png       the guide over the 4x reference (or the original)
  <Font>_map.csv              every glyph: character, code, box at 1x and 4x,
                              left offset, drawn width, advance
  README.txt                  how to turn a drawing into a replacement

Draw on a 4x canvas (e.g. 2048x2048 for BronzeBigFont_30) keeping every glyph
inside its box, and save it as textures\\<Font>_<HASH>.png next to amped3.exe;
it then replaces the font (and wins over Sharp Fonts).

    python tools/textures/font_templates.py --game "C:\\...\\Amped 3\\game" --out font-templates
    [--reference <folder with <Font>.png 4x images to draw over>]

Needs Pillow and numpy.
"""
import argparse
import csv
import io
import os
import struct
import zipfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

FONTS = [  # name, package, hash (texture_replace.cpp ContentHash), use
    ('BronzeBigFont_30', 'Base', '55C4E466D81E083A', 'titles and menu items'),
    ('NewAgency_18', 'Base', '7DE883665797A1E4', 'small text, button legends, button icons'),
    ('XMRadio_supernatural_18', 'Base', 'CD6AA4AF88CEE017', 'the XM radio display'),
    ('CrashNumbers', 'Game', 'E83086C4D1D718C5', 'crash scores'),
    ('NewFunkNumbers', 'Game', '283E0BB38D47E7E9', 'trick scores'),
]
SCALE = 4

# The button glyphs, by code (the game's string tables write them as {_A} etc.).
SYMBOLS = {
    0x01: 'missing', 0x09: 'A', 0x0B: 'B', 0x0C: 'X', 0x0D: 'Y', 0x0E: 'Back', 0x0F: 'Start',
    0x10: 'Guide', 0x11: 'D-pad', 0x12: 'L stick', 0x13: 'R stick', 0x14: 'LB', 0x15: 'RB',
    0x16: 'LT', 0x17: 'RT', 0x18: 'Up', 0x19: 'Down', 0x1A: 'Left', 0x1B: 'Right',
    0x1C: 'Up-left', 0x1D: 'Up-right', 0x1E: 'Down-left', 0x1F: 'Down-right',
}


# --- Xbox 360 texture decoding (XPR2, tiled DXT) -------------------------------

def _tiled(x, y, w, lb):
    aw = (w + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << lb
    o = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
    return (((o & ~511) << 3) + ((o & 448) << 2) + (o & 63) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb


def _c565(c):
    return np.array([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31])


def decode_xpr(data):
    hdr = struct.unpack('>I', data[4:8])[0]
    off = struct.unpack('>I', data[20:24])[0]
    f = struct.unpack('>6I', data[12 + off + 16:12 + off + 40])
    fmt, endian = f[1] & 63, (f[1] >> 6) & 3
    w, h = (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1
    base = 12 + hdr + (f[1] & 0xFFFFF000)
    assert fmt == 0x13, 'fonts are DXT3'
    bw, bh = w // 4, h // 4
    img = np.zeros((h, w, 4), np.uint8)
    for by in range(bh):
        for bx in range(bw):
            o = base + _tiled(bx, by, bw, 4) * 16
            b = bytearray(data[o:o + 16])
            if endian == 1:
                for k in range(0, 16, 2):
                    b[k], b[k + 1] = b[k + 1], b[k]
            c0, c1, idx = struct.unpack('<HHI', bytes(b[8:16]))
            p0, p1 = _c565(c0), _c565(c1)
            pal = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3]
            av = struct.unpack('<Q', bytes(b[:8]))[0]
            for i in range(16):
                img[by * 4 + i // 4, bx * 4 + i % 4, :3] = pal[(idx >> (2 * i)) & 3]
                img[by * 4 + i // 4, bx * 4 + i % 4, 3] = ((av >> (4 * i)) & 15) * 17
    return Image.fromarray(img, 'RGBA')


def parse_abc(data):
    _ver, cell, tw, th, ascent, nranges = struct.unpack('<6I', data[:24])
    codes = []
    for i in range(nranges):
        a, b = struct.unpack('<HH', data[24 + 4 * i:28 + 4 * i])
        codes += range(a, b + 1)
    o = 24 + 4 * nranges
    count = struct.unpack('<I', data[o:o + 4])[0]
    o += 4
    glyphs = []
    for i in range(count):
        u0, v0, u1, v1, offset, width, advance = struct.unpack('<4f3h', data[o + 28 * i:o + 28 * i + 22])
        glyphs.append(dict(code=codes[i], x=round(u0 * tw), y=round(v0 * th), x1=round(u1 * tw),
                           y1=round(v1 * th), offset=offset, width=width, advance=advance))
    return dict(cell=cell, ascent=ascent, width=tw, height=th, glyphs=glyphs)


def label(code):
    if code in SYMBOLS:
        return SYMBOLS[code]
    ch = chr(code)
    return ch if ch.isprintable() and ch != ' ' else 'space' if ch == ' ' else f'U+{code:04X}'


def find_font():
    for p in ['C:\\Windows\\Fonts\\arial.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf']:
        if os.path.exists(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True, help='the game folder (with GuiPackages)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--reference', help='folder of <Font>.png 4x images to preview the guide over')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    ttf = find_font()
    readme = [
        'Amped 3 font templates', '======================', '',
        'Draw each font on a canvas 4x the original (the "4x size" below), every glyph',
        'inside its box in <Font>_4x_guide.png (use it as a layer over your drawing).',
        'The game places each glyph by its box: its left edge plus the "offset" column',
        'in the map, and advances by "advance" (1x pixels) to the next character.',
        'White text with a black outline is what the game draws; it tints the white',
        '(e.g. pink scores), so keep letters white and outlines black. Transparency is',
        'the background.', '',
        'Save the finished image as textures\\<Font>_<HASH>.png next to amped3.exe (any',
        'size works as long as the layout is the original scaled evenly; 4x suggested).',
        'It then replaces the font and takes precedence over Sharp Fonts.', '',
    ]
    for name, package, h, use in FONTS:
        with zipfile.ZipFile(os.path.join(args.game, 'GuiPackages', package + '.rui')) as z:
            abc = parse_abc(z.read(f'Fonts/{name}.abc'))
        with zipfile.ZipFile(os.path.join(args.game, 'GuiPackages', package + '.ruirr')) as z:
            atlas = decode_xpr(z.read(f'Fonts/{name}.xpr'))
        atlas.save(os.path.join(args.out, f'{name}_1x_original.png'))
        W, H = atlas.width * SCALE, atlas.height * SCALE
        guide = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(guide)
        small = ImageFont.truetype(ttf, 9 * SCALE // 2) if ttf else ImageFont.load_default()
        rows = []
        for g in abc['glyphs']:
            x0, y0, x1, y1 = (v * SCALE for v in (g['x'], g['y'], g['x1'], g['y1']))
            d.rectangle([x0, y0, x1 - 1, y1 - 1], outline=(0, 230, 255, 255), width=2)
            by = y0 + abc['ascent'] * SCALE
            d.line([x0, by, x1 - 1, by], fill=(255, 60, 160, 200), width=1)
            # Name and code under the box's bottom-left; the code alone where
            # the box is too narrow for both.
            text = f'{label(g["code"])} {g["code"]:02X}'
            if d.textlength(text, font=small) > x1 - x0 - 4:
                text = f'{g["code"]:02X}'
            d.text((x0 + 3, y1 - 3), text, font=small, fill=(255, 255, 0, 255), anchor='ld',
                   stroke_width=2, stroke_fill=(0, 0, 0, 255))
            rows.append([label(g['code']), f'0x{g["code"]:04X}', g['x'], g['y'], g['x1'] - g['x'],
                         g['y1'] - g['y'], x0, y0, x1 - x0, y1 - y0, g['offset'], g['width'],
                         g['advance']])
        guide.save(os.path.join(args.out, f'{name}_4x_guide.png'))
        base = None
        if args.reference and os.path.exists(os.path.join(args.reference, name + '.png')):
            base = Image.open(os.path.join(args.reference, name + '.png')).convert('RGBA')
            if base.size != (W, H):
                base = None
        if base is None:
            base = atlas.resize((W, H), Image.NEAREST)
        bg = Image.new('RGBA', (W, H), (60, 90, 140, 255))
        bg.alpha_composite(base)
        bg.alpha_composite(guide)
        bg.convert('RGB').save(os.path.join(args.out, f'{name}_4x_preview.png'))
        with open(os.path.join(args.out, f'{name}_map.csv'), 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['char', 'code', 'x', 'y', 'w', 'h', 'x_4x', 'y_4x', 'w_4x', 'h_4x',
                        'offset', 'width', 'advance'])
            w.writerows(rows)
        readme += [f'{name}  ({use})',
                   f'    original {atlas.width}x{atlas.height}, 4x size {W}x{H}, '
                   f'{len(abc["glyphs"])} glyphs, line height {abc["cell"]}, baseline {abc["ascent"]}',
                   f'    save as: textures\\{name}_{h}.png', '']
        print(name, atlas.size, len(abc['glyphs']))
    readme += ['In the guide: cyan = glyph box, pink = baseline, yellow = character and code.',
               'Button icons are glyphs too (codes 09 and 0B-1F in NewAgency_18 and BronzeBigFont_30).']
    with open(os.path.join(args.out, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(readme) + '\n')


if __name__ == '__main__':
    main()
