#!/usr/bin/env python3
"""Decode the textures saved by an F10 frame capture (capture_resources) to PNG.

Usage: texdecode.py <frame folder> [<out folder>] [--only NAME ...]

Reads resources.txt in the frame folder, untiles each texture's base level
(Xenos 2D tiling), undoes the GPU endian swap and decodes the format. This is
the reference implementation for the native renderer's texture loader (step 2):
if these PNGs match the game, the same steps on the GPU will too.

Supported formats: k_8, k_8_8, k_5_6_5, k_4_4_4_4, k_1_5_5_5, k_8_8_8_8,
k_DXT1, k_DXT2_3, k_DXT4_5, k_DXN, k_DXT5A, k_DXT3A. Others are reported and
skipped.
"""
import os
import re
import sys

import numpy as np
from PIL import Image

# Block width, block height, bits per pixel per format (SDK info_formats.cpp).
BLOCK = {
    2: (1, 1, 8), 3: (1, 1, 16), 4: (1, 1, 16), 6: (1, 1, 32), 10: (1, 1, 16),
    15: (1, 1, 16), 18: (4, 4, 4), 19: (4, 4, 8), 20: (4, 4, 8), 49: (4, 4, 8),
    58: (4, 4, 4), 59: (4, 4, 4),
}
NAMES = {2: 'k_8', 3: 'k_1_5_5_5', 4: 'k_5_6_5', 6: 'k_8_8_8_8', 10: 'k_8_8', 15: 'k_4_4_4_4',
         18: 'k_DXT1', 19: 'k_DXT2_3', 20: 'k_DXT4_5', 49: 'k_DXN', 58: 'k_DXT3A',
         59: 'k_DXT5A'}


def parse_fetch(words):
    d = [int(w, 16) for w in words]
    f = {
        'tiled': (d[0] >> 31) & 1,
        'pitch': ((d[0] >> 22) & 0x1FF) * 32,
        'format': d[1] & 63,
        'endian': (d[1] >> 6) & 3,
        'width': (d[2] & 0x1FFF) + 1,
        'height': ((d[2] >> 13) & 0x1FFF) + 1,
        'swizzle': (d[3] >> 1) & 0xFFF,
        'dimension': (d[5] >> 9) & 3,
        'packed_mips': (d[5] >> 11) & 1,
    }
    return f


def endian_swap(data, mode):
    a = np.frombuffer(data, dtype=np.uint8)
    n = len(a) // 4 * 4
    a = a[:n]
    if mode == 1:  # 8-in-16
        return a.reshape(-1, 2)[:, ::-1].reshape(-1).tobytes()
    if mode == 2:  # 8-in-32
        return a.reshape(-1, 4)[:, ::-1].reshape(-1).tobytes()
    if mode == 3:  # 16-in-32
        return a.reshape(-1, 2, 2)[:, ::-1, :].reshape(-1).tobytes()
    return a.tobytes()


def tiled_offsets(width_blocks, height_blocks, pitch_blocks, log2_bpb):
    """Byte offset of each block (y, x) in a 2D tiled Xenos texture."""
    y = np.arange(height_blocks, dtype=np.int64)[:, None]
    x = np.arange(width_blocks, dtype=np.int64)[None, :]
    aligned_pitch = (pitch_blocks + 31) & ~31
    macro = ((y >> 5) * (aligned_pitch >> 5)) << (log2_bpb + 7)
    micro = ((y & 6) << 2) << log2_bpb
    outer = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log2_bpb)) + ((y & 1) << 4)
    macro_x = (x >> 5) << (log2_bpb + 7)
    micro_x = (x & 7) << log2_bpb
    offset = outer + (macro_x + ((micro_x & ~15) << 1) + (micro_x & 15))
    return (((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def gather_blocks(data, f):
    bw, bh, bpp = BLOCK[f['format']]
    bpb = bw * bh * bpp // 8
    wb = (f['width'] + bw - 1) // bw
    hb = (f['height'] + bh - 1) // bh
    pitch_b = max((f['pitch'] + bw - 1) // bw, wb)
    buf = np.frombuffer(data, dtype=np.uint8)
    if f['tiled']:
        # Packed mips: with the shorter side 16 texels or less, the base level
        # sits 16 texels into a shared 32x32 tile, across the longer side.
        ox = oy = 0
        lw = (f['width'] - 1).bit_length()
        lh = (f['height'] - 1).bit_length()
        if f.get('packed_mips') and min(lw, lh) <= 4:
            if lw > lh:
                oy = 16 // bh
            else:
                ox = 16 // bw
        tile_pitch = max(pitch_b, (ox + wb + 31) & ~31)
        offs = tiled_offsets(wb + ox, hb + oy, tile_pitch, bpb.bit_length() - 1)[oy:, ox:]
    else:
        row = (pitch_b * bpb + 255) & ~255
        offs = (np.arange(hb)[:, None] * row + np.arange(wb)[None, :] * bpb).astype(np.int64)
    need = offs.max() + bpb
    if need > len(buf):
        buf = np.concatenate([buf, np.zeros(need - len(buf), np.uint8)])
    idx = offs[..., None] + np.arange(bpb)
    return buf[idx], wb, hb  # [hb, wb, bpb]


def rgb565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def decode_dxt_color(blk, punch):
    """blk: [..., 8] little-endian DXT1 colour block -> [..., 16, 4]."""
    c0 = blk[..., 0].astype(np.int32) | (blk[..., 1].astype(np.int32) << 8)
    c1 = blk[..., 2].astype(np.int32) | (blk[..., 3].astype(np.int32) << 8)
    bits = (blk[..., 4].astype(np.uint32) | (blk[..., 5].astype(np.uint32) << 8) |
            (blk[..., 6].astype(np.uint32) << 16) | (blk[..., 7].astype(np.uint32) << 24))
    p0, p1 = rgb565(c0), rgb565(c1)
    four = (c0 > c1)[..., None] | (not punch)
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], -2)  # [..., 4, 3]
    alpha = np.stack([np.full(c0.shape, 255), np.full(c0.shape, 255), np.full(c0.shape, 255),
                      np.where(four[..., 0], 255, 0)], -1)
    sel = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], -1).astype(np.int64)
    rgb = np.take_along_axis(pal, sel[..., None].repeat(3, -1), -2)
    a = np.take_along_axis(alpha, sel, -1)
    return np.concatenate([rgb, a[..., None]], -1)


def decode_dxt5_alpha(blk):
    a0 = blk[..., 0].astype(np.int32)
    a1 = blk[..., 1].astype(np.int32)
    bits = np.zeros(a0.shape, np.uint64)
    for i in range(6):
        bits |= blk[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    pal = [a0, a1]
    for i in range(1, 7):
        pal.append(np.where(a0 > a1, ((7 - i) * a0 + i * a1) // 7,
                            np.where(i < 5, ((5 - i) * a0 + i * a1) // 5, 0 if i == 5 else 255)))
    pal = np.stack(pal, -1)
    sel = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int64)
                    for i in range(16)], -1)
    return np.take_along_axis(pal, sel, -1)


def decode_dxt3_alpha(blk):
    out = []
    for i in range(16):
        byte = blk[..., i // 2].astype(np.int32)
        out.append(((byte >> (4 * (i & 1))) & 15) * 17)
    return np.stack(out, -1)


def blocks_to_image(px, wb, hb, bw, bh):
    """px: [hb, wb, bw*bh, C] -> [hb*bh, wb*bw, C]."""
    c = px.shape[-1]
    return px.reshape(hb, wb, bh, bw, c).transpose(0, 2, 1, 3, 4).reshape(hb * bh, wb * bw, c)


def decode(data, f):
    fmt = f['format']
    if fmt not in BLOCK:
        raise ValueError(f'format {fmt} not supported')
    data = endian_swap(data, f['endian'])
    blocks, wb, hb = gather_blocks(data, f)
    bw, bh, _ = BLOCK[fmt]
    if fmt == 18:
        px = decode_dxt_color(blocks, True)
    elif fmt in (19, 20):
        px = decode_dxt_color(blocks[..., 8:16], False)
        px[..., 3] = decode_dxt3_alpha(blocks[..., 0:8]) if fmt == 19 else decode_dxt5_alpha(
            blocks[..., 0:8])
    elif fmt == 49:  # DXN: two DXT5-alpha channels (X, Y)
        r = decode_dxt5_alpha(blocks[..., 0:8])
        g = decode_dxt5_alpha(blocks[..., 8:16])
        px = np.stack([r, g, np.full(r.shape, 128), np.full(r.shape, 255)], -1)
    elif fmt in (58, 59):
        a = decode_dxt3_alpha(blocks) if fmt == 58 else decode_dxt5_alpha(blocks)
        px = np.stack([a, a, a, np.full(a.shape, 255)], -1)
    else:
        b = blocks.astype(np.uint32)
        if fmt == 2:
            v = b[..., 0]
            px = np.stack([v, v, v, np.full(v.shape, 255)], -1)
        elif fmt == 10:
            px = np.stack([b[..., 1], b[..., 0], np.zeros_like(b[..., 0]),
                           np.full(b[..., 0].shape, 255)], -1)
        elif fmt == 6:
            v = b[..., 0] | (b[..., 1] << 8) | (b[..., 2] << 16) | (b[..., 3] << 24)
            # Xenos k_8_8_8_8: X in bits 0-7 ... W in 24-31; D3D maps it as ARGB.
            px = np.stack([(v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24], -1)
        else:
            v = b[..., 0] | (b[..., 1] << 8)
            if fmt == 4:
                px = np.concatenate([rgb565(v.astype(np.int32)), np.full(v.shape + (1,), 255)], -1)
            elif fmt == 3:
                px = np.stack([((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31,
                               (v & 31) * 255 // 31, (v >> 15) * 255], -1)
            else:  # 4_4_4_4
                px = np.stack([((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17,
                               ((v >> 12) & 15) * 17], -1)
        px = px[..., None, :]
    img = blocks_to_image(px, wb, hb, bw, bh)[: f['height'], : f['width']]
    return np.clip(img, 0, 255).astype(np.uint8)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    folder = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else os.path.join(
        folder, 'png')
    only = set(sys.argv[sys.argv.index('--only') + 1:]) if '--only' in sys.argv else None
    os.makedirs(out, exist_ok=True)
    done = skipped = 0
    for line in open(os.path.join(folder, 'resources.txt')):
        if not line.startswith('tex_'):
            continue
        name = line.split()[0]
        if only and name not in only:
            continue
        f = parse_fetch(re.search(r'fetch=\[([^\]]+)\]', line).group(1).split())
        path = os.path.join(folder, name + '.base.bin')
        if not os.path.exists(path) or f['dimension'] != 1:
            skipped += 1
            continue
        try:
            img = decode(open(path, 'rb').read(), f)
        except ValueError as e:
            print(f'{name}: {e}')
            skipped += 1
            continue
        Image.fromarray(img, 'RGBA').save(
            os.path.join(out, f"{name}_{NAMES.get(f['format'], f['format'])}_"
                              f"{f['width']}x{f['height']}.png"))
        done += 1
    print(f'{done} textures decoded to {out}, {skipped} skipped')
    return 0


if __name__ == '__main__':
    sys.exit(main())
