#!/usr/bin/env python3
"""Builds the logo replacements in assets/local/textures from
assets/local/amped3_logo.png.

assets/local is not part of the published sources (git ignores it): the logo
is the game's trademarked artwork, so it isn't covered by this project's
license. The build copies assets/local/textures next to the exe when present.

The native renderer swaps a game texture for textures/<name>_<hash>.png when
one is there (see src/native/texture_replace.cpp). This places the new logo
where the game's own logo sits in each texture, at 4x their size:

  AmpedBoxLogo (Shell.ruirr, 512x256 DXT3): the title screen's logo.
  background.xtx (diesel/assets/assets.res, 1024x512 DXT1): the loading
    screen at start-up, logo on black. Shown stretched to 16:9, so the new
    logo is widened by 2/(16/9) = 1.125 to look right on screen.

The boxes are the original logos' extents (measured from the game's
textures). Needs Pillow and numpy:  python tools/textures/make_logo_textures.py
"""
import os
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'assets', 'local', 'amped3_logo.png')
OUT = os.path.join(ROOT, 'assets', 'local', 'textures')

# name, hash, texture size, scale, original logo box (x0, y0, x1, y1 inclusive,
# texture pixels), horizontal stretch on screen, background
TARGETS = [
    ('AmpedBoxLogo', '3B66B93CF489E918', (512, 256), 4, (3, 26, 509, 228), 1.0, None),
    ('LoadingScreen', '4F1CA4AEA190FC18', (1024, 512), 4, (134, 107, 897, 376), 1.125, (0, 0, 0)),
]


def main():
    logo = Image.open(SRC).convert('RGBA')
    alpha = np.asarray(logo)[..., 3]
    ys, xs = np.where(alpha > 8)
    logo = logo.crop((int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1))
    aspect = logo.width / logo.height
    os.makedirs(OUT, exist_ok=True)
    for name, h, (tw, th), scale, (x0, y0, x1, y1), stretch, bg in TARGETS:
        cw, ch = tw * scale, th * scale
        bw, bh = (x1 - x0 + 1) * scale, (y1 - y0 + 1) * scale
        cx, cy = (x0 + x1 + 1) / 2 * scale, (y0 + y1 + 1) / 2 * scale
        # Fit inside the old logo's box as it looks on screen.
        disp_w = bw / stretch
        if disp_w / bh > aspect:
            nh = bh
            nw = bh * aspect * stretch
        else:
            nw = bw
            nh = disp_w / aspect
        nw, nh = int(round(nw)), int(round(nh))
        scaled = logo.convert('RGBa').resize((nw, nh), Image.LANCZOS).convert('RGBA')
        canvas = Image.new('RGBA', (cw, ch), (0, 0, 0, 0) if bg is None else bg + (255,))
        canvas.alpha_composite(scaled, (int(round(cx - nw / 2)), int(round(cy - nh / 2))))
        if bg is not None:
            canvas = canvas.convert('RGB')
        path = os.path.join(OUT, f'{name}_{h}.png')
        canvas.save(path, optimize=True)
        print(path, canvas.size, (nw, nh))


if __name__ == '__main__':
    main()
