// Xenos texture decoding to RGBA8. See xenos_texture.h.
//
// Component order: each format decodes to its "natural" XYZW (X = red, in
// the lowest bits of packed formats, as in the SDK's texture_cache.cpp host
// formats), with missing components replicated the way the SDK's host swizzles
// do (k_8 = RRRR, k_8_8 = RGGG, k_5_6_5 = RGBB). The fetch constant's swizzle
// then picks from those, or 0 / 1.

#include "xenos_texture.h"

#include <algorithm>
#include <cstring>

namespace amped3::native {
namespace {


// Block dimensions and bits per texel (SDK info_formats.cpp).
bool GetFormatInfo(uint32_t format, uint32_t* bw, uint32_t* bh, uint32_t* bytes_per_block) {
  switch (format) {
    case 2: *bw = *bh = 1; *bytes_per_block = 1; return true;   // k_8
    case 5: *bw = *bh = 1; *bytes_per_block = 2; return true;   // k_6_5_5
    case 8:                                                      // k_8_A
    case 9: *bw = *bh = 1; *bytes_per_block = 1; return true;   // k_8_B
    case 11:                                                     // k_Cr_Y1_Cb_Y0_REP
    case 12: *bw = 2; *bh = 1; *bytes_per_block = 4; return true;  // k_Y1_Cr_Y0_Cb_REP
    case 14: *bw = *bh = 1; *bytes_per_block = 4; return true;  // k_8_8_8_8_A
    case 16:                                                     // k_10_11_11
    case 17: *bw = *bh = 1; *bytes_per_block = 4; return true;  // k_11_11_10
    case 24: *bw = *bh = 1; *bytes_per_block = 2; return true;  // k_16
    case 25: *bw = *bh = 1; *bytes_per_block = 4; return true;  // k_16_16
    case 26: *bw = *bh = 1; *bytes_per_block = 8; return true;  // k_16_16_16_16
    case 30: *bw = *bh = 1; *bytes_per_block = 2; return true;  // k_16_FLOAT
    case 31: *bw = *bh = 1; *bytes_per_block = 4; return true;  // k_16_16_FLOAT
    case 32: *bw = *bh = 1; *bytes_per_block = 8; return true;  // k_16_16_16_16_FLOAT
    case 36: *bw = *bh = 1; *bytes_per_block = 4; return true;  // k_32_FLOAT
    case 37: *bw = *bh = 1; *bytes_per_block = 8; return true;  // k_32_32_FLOAT
    case 38: *bw = *bh = 1; *bytes_per_block = 16; return true; // k_32_32_32_32_FLOAT
    case 3: *bw = *bh = 1; *bytes_per_block = 2; return true;   // k_1_5_5_5
    case 4: *bw = *bh = 1; *bytes_per_block = 2; return true;   // k_5_6_5
    case 6: *bw = *bh = 1; *bytes_per_block = 4; return true;   // k_8_8_8_8
    case 7: *bw = *bh = 1; *bytes_per_block = 4; return true;   // k_2_10_10_10
    case 10: *bw = *bh = 1; *bytes_per_block = 2; return true;  // k_8_8
    case 15: *bw = *bh = 1; *bytes_per_block = 2; return true;  // k_4_4_4_4
    case 18: *bw = *bh = 4; *bytes_per_block = 8; return true;  // k_DXT1
    case 19: *bw = *bh = 4; *bytes_per_block = 16; return true; // k_DXT2_3
    case 20: *bw = *bh = 4; *bytes_per_block = 16; return true; // k_DXT4_5
    case 23: *bw = *bh = 1; *bytes_per_block = 4; return true;  // k_24_8_FLOAT (not decoded)
    case 49: *bw = *bh = 4; *bytes_per_block = 16; return true; // k_DXN
    case 58: *bw = *bh = 4; *bytes_per_block = 8; return true;  // k_DXT3A
    case 59: *bw = *bh = 4; *bytes_per_block = 8; return true;  // k_DXT5A
    default: return false;
  }
}

uint32_t Log2(uint32_t v) {
  uint32_t r = 0;
  while ((1u << (r + 1)) <= v) ++r;
  return r;
}

uint32_t CeilLog2(uint32_t v) {
  uint32_t r = 0;
  while ((1u << r) < v) ++r;
  return r;
}

// XGAddress2DTiledOffset, in blocks -> bytes.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t log2_bpb) {
  const uint32_t aligned_pitch = (pitch_blocks + 31) & ~31u;
  const uint32_t macro = ((y >> 5) * (aligned_pitch >> 5)) << (log2_bpb + 7);
  const uint32_t micro = ((y & 6) << 2) << log2_bpb;
  const uint32_t outer =
      macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log2_bpb)) + ((y & 1) << 4);
  const uint32_t macro_x = (x >> 5) << (log2_bpb + 7);
  const uint32_t micro_x = (x & 7) << log2_bpb;
  const uint32_t offset = outer + (macro_x + ((micro_x & ~15u) << 1) + (micro_x & 15));
  return ((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

void Swap(uint8_t* data, size_t size, uint32_t endian) {
  switch (endian) {
    case 1:  // 8-in-16
      for (size_t i = 0; i + 1 < size; i += 2) std::swap(data[i], data[i + 1]);
      break;
    case 2:  // 8-in-32
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case 3:  // 16-in-32
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Unsigned normalized 16-bit -> 8-bit (rounded).
inline uint8_t Unorm16(uint32_t v) { return uint8_t((v * 255u + 32767u) / 65535u); }

inline uint8_t FloatByte(float f) {
  if (!(f > 0.0f)) return 0;  // also NaN
  if (f >= 1.0f) return 255;
  return uint8_t(f * 255.0f + 0.5f);
}

inline float Half(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31, exp = (h >> 10) & 31, man = h & 1023;
  uint32_t bits;
  if (exp == 0) {
    if (man != 0) {  // subnormal: man * 2^-24
      const float f = float(man) / 16777216.0f;
      return sign ? -f : f;
    }
    bits = sign;
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (man << 13);
  } else {
    bits = sign | ((exp + 112) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

inline float F32(const uint8_t* p) {
  const uint32_t bits = Le32(p);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint8_t Expand(uint32_t v, uint32_t bits) { return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

// Natural XYZW of a 4x4 block -> out[16][4].
void DecodeDxtColor(const uint8_t* b, bool dxt1, uint8_t out[16][4]) {
  const uint16_t c0 = Le16(b), c1 = Le16(b + 2);
  const uint32_t bits = Le32(b + 4);
  uint8_t pal[4][4];
  auto rgb565 = [](uint16_t c, uint8_t* o) {
    o[0] = Expand((c >> 11) & 31, 5);
    o[1] = Expand((c >> 5) & 63, 6);
    o[2] = Expand(c & 31, 5);
    o[3] = 255;
  };
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  if (!dxt1 || c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
      pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = 0;
    pal[3][3] = 0;
  }
  for (int i = 0; i < 16; ++i) std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

void DecodeDxt5Alpha(const uint8_t* b, uint8_t out[16]) {
  const uint32_t a0 = b[0], a1 = b[1];
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  uint8_t pal[8];
  pal[0] = uint8_t(a0);
  pal[1] = uint8_t(a1);
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i < 5; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
  for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

void DecodeDxt3Alpha(const uint8_t* b, uint8_t out[16]) {
  for (int i = 0; i < 16; ++i) out[i] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
}

// Where one level's blocks are, relative to the start of its data.
struct LevelPlacement {
  uint32_t width = 0, height = 0;  // texels
  size_t offset_bytes = 0;
  uint32_t pitch_blocks = 0;        // tiled: row pitch in blocks
  uint32_t row_bytes = 0;           // linear: row pitch in bytes
  uint32_t off_bx = 0, off_by = 0;  // position in the packed mip tail, in blocks
};

uint32_t NextPow2(uint32_t v) { return 1u << CeilLog2(v); }
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// First level stored in the packed mip tail (SDK texture util
// GetPackedMipLevel).
uint32_t PackedMipLevel(uint32_t width, uint32_t height) {
  const uint32_t log2_size = CeilLog2(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

// Block offset of `mip` inside the packed mip tail of a width x height
// texture (SDK texture util GetPackedMipOffset, 2D only).
void PackedMipOffset(uint32_t width, uint32_t height, uint32_t mip, uint32_t bw, uint32_t bh,
                     uint32_t* x_blocks, uint32_t* y_blocks) {
  const uint32_t log2_width = CeilLog2(width), log2_height = CeilLog2(height);
  const uint32_t log2_size = std::min(log2_width, log2_height);
  *x_blocks = *y_blocks = 0;
  if (log2_size > 4 + mip) return;
  const uint32_t packed_mip_base = log2_size > 4 ? log2_size - 4 : 0;
  const uint32_t packed_mip = mip - packed_mip_base;
  uint32_t x = 0, y = 0;
  if (packed_mip < 3) {
    if (log2_width > log2_height) {
      y = 16 >> packed_mip;  // wider than tall: laid out vertically
    } else {
      x = 16 >> packed_mip;
    }
  } else if (log2_width > log2_height) {
    x = (1u << (log2_width - packed_mip_base)) >> (packed_mip - 2);
  } else {
    y = (1u << (log2_height - packed_mip_base)) >> (packed_mip - 2);
  }
  *x_blocks = x / bw;
  *y_blocks = y / bh;
}

// Placement of `level` (0 = base, in the base data; >= 1, in the mip data),
// following the SDK's GetGuestTextureLayout: mip strides come from a
// power-of-two base size, each level is 32x32-block and 4 KB aligned, and the
// levels from the packed level down share one tile.
LevelPlacement PlaceLevel(const TextureFetch& fetch, uint32_t level, uint32_t bw, uint32_t bh,
                          uint32_t bpb) {
  LevelPlacement L;
  const uint32_t w = fetch.width();
  const uint32_t h = fetch.dimension() == 0 ? 1 : fetch.height();
  const bool tiled = fetch.tiled();
  const uint32_t packed = fetch.packed_mips() ? PackedMipLevel(w, h) : UINT32_MAX;
  L.width = std::max(w >> level, 1u);
  L.height = std::max(h >> level, 1u);
  // Mip row pitch in blocks (tile aligned) and rows (tile aligned).
  auto mip_pitch = [&](uint32_t stored) {
    return AlignUp(AlignUp(std::max(NextPow2(w) >> stored, 1u), bw) / bw, 32);
  };
  auto mip_rows = [&](uint32_t stored) {
    return AlignUp(AlignUp(std::max(NextPow2(h) >> stored, 1u), bh) / bh, 32);
  };
  if (level == 0) {
    const uint32_t pitch = std::max((fetch.pitch_texels() + bw - 1) / bw, (w + bw - 1) / bw);
    L.pitch_blocks = AlignUp(pitch, 32);
    L.row_bytes = AlignUp(pitch * bpb, 256);
  } else {
    const uint32_t stored = std::min(level, packed);
    size_t offset = 0;
    for (uint32_t s = 1; s < stored; ++s) {
      const uint32_t row_bytes = tiled ? mip_pitch(s) * bpb : AlignUp(mip_pitch(s) * bpb, 256);
      offset += AlignUp(row_bytes * mip_rows(s), 4096);
    }
    L.offset_bytes = offset;
    L.pitch_blocks = mip_pitch(stored);
    L.row_bytes = AlignUp(L.pitch_blocks * bpb, 256);
  }
  if (level >= packed) {
    PackedMipOffset(w, h, level, bw, bh, &L.off_bx, &L.off_by);
    if (tiled) {
      L.pitch_blocks = std::max(L.pitch_blocks, AlignUp(L.off_bx + (L.width + bw - 1) / bw, 32));
    }
  }
  return L;
}

// Decodes one level (from endian-swapped data) to RGBA8 with the fetch
// swizzle.
void DecodeLevel(const TextureFetch& fetch, uint32_t format, uint32_t bw, uint32_t bh,
                 uint32_t bpb, const uint8_t* buf, size_t size, const LevelPlacement& L,
                 uint8_t* rgba) {
  const bool tiled = fetch.tiled();
  const uint32_t wb = (L.width + bw - 1) / bw;
  const uint32_t hb = (L.height + bh - 1) / bh;
  const uint32_t log2_bpb = Log2(bpb);
  uint8_t natural[16][4];
  for (uint32_t by = 0; by < hb; ++by) {
    for (uint32_t bx = 0; bx < wb; ++bx) {
      const size_t offset =
          L.offset_bytes +
          (tiled ? TiledOffset(bx + L.off_bx, by + L.off_by, L.pitch_blocks, log2_bpb)
                 : size_t(by + L.off_by) * L.row_bytes + size_t(bx + L.off_bx) * bpb);
      if (offset + bpb > size) continue;
      const uint8_t* b = buf + offset;
      switch (format) {
        case 2:
        case 8:
        case 9:
          natural[0][0] = natural[0][1] = natural[0][2] = natural[0][3] = b[0];
          break;
        case 5: {  // k_6_5_5: X = 5 bits (low), Y = 5, Z = 6 (high); W = Z
          const uint16_t v = Le16(b);
          natural[0][0] = Expand(v & 31, 5);
          natural[0][1] = Expand((v >> 5) & 31, 5);
          natural[0][2] = natural[0][3] = Expand((v >> 10) & 63, 6);
          break;
        }
        case 11:
        case 12: {
          // Packed 4:2:2 YUV (videos): two texels per 32-bit block sharing Cb
          // and Cr. Sampling returns (Cr, Y, Cb) with W = Cb, as the SDK's
          // GRGB / RGBG host formats with the RGBB swizzle; the game's pixel
          // shader converts to RGB.
          const uint32_t v = Le32(b);
          uint8_t y0, y1, cb, cr;
          if (format == 12) {  // Y1 Cr Y0 Cb, from the high byte down
            cb = uint8_t(v);
            y0 = uint8_t(v >> 8);
            cr = uint8_t(v >> 16);
            y1 = uint8_t(v >> 24);
          } else {  // Cr Y1 Cb Y0
            y0 = uint8_t(v);
            cb = uint8_t(v >> 8);
            y1 = uint8_t(v >> 16);
            cr = uint8_t(v >> 24);
          }
          const uint8_t ys[2] = {y0, y1};
          for (int t = 0; t < 2; ++t) {
            natural[t][0] = cr;
            natural[t][1] = ys[t];
            natural[t][2] = natural[t][3] = cb;
          }
          break;
        }
        case 14: {
          const uint32_t v = Le32(b);
          for (int c = 0; c < 4; ++c) natural[0][c] = uint8_t(v >> (8 * c));
          break;
        }
        case 16:    // k_10_11_11: X = 11 bits (low), Y = 11, Z = 10
        case 17: {  // k_11_11_10: X = 10 bits (low), Y = 11, Z = 11
          const uint32_t v = Le32(b);
          const uint32_t xb = format == 16 ? 11 : 10, yb = 11, zb = format == 16 ? 10 : 11;
          natural[0][0] = Expand(v & ((1u << xb) - 1), xb);
          natural[0][1] = Expand((v >> xb) & ((1u << yb) - 1), yb);
          natural[0][2] = natural[0][3] = Expand((v >> (xb + yb)) & ((1u << zb) - 1), zb);
          break;
        }
        case 24:
          natural[0][0] = natural[0][1] = natural[0][2] = natural[0][3] = Unorm16(Le16(b));
          break;
        case 25:
          natural[0][0] = Unorm16(Le16(b));
          natural[0][1] = natural[0][2] = natural[0][3] = Unorm16(Le16(b + 2));
          break;
        case 26:
          for (int c = 0; c < 4; ++c) natural[0][c] = Unorm16(Le16(b + 2 * c));
          break;
        case 30:
          natural[0][0] = natural[0][1] = natural[0][2] = natural[0][3] = FloatByte(Half(Le16(b)));
          break;
        case 31:
          natural[0][0] = FloatByte(Half(Le16(b)));
          natural[0][1] = natural[0][2] = natural[0][3] = FloatByte(Half(Le16(b + 2)));
          break;
        case 32:
          for (int c = 0; c < 4; ++c) natural[0][c] = FloatByte(Half(Le16(b + 2 * c)));
          break;
        case 36:
          natural[0][0] = natural[0][1] = natural[0][2] = natural[0][3] = FloatByte(F32(b));
          break;
        case 37:
          natural[0][0] = FloatByte(F32(b));
          natural[0][1] = natural[0][2] = natural[0][3] = FloatByte(F32(b + 4));
          break;
        case 38:
          for (int c = 0; c < 4; ++c) natural[0][c] = FloatByte(F32(b + 4 * c));
          break;
        case 3: {
          const uint16_t v = Le16(b);
          natural[0][0] = Expand(v & 31, 5);
          natural[0][1] = Expand((v >> 5) & 31, 5);
          natural[0][2] = Expand((v >> 10) & 31, 5);
          natural[0][3] = (v >> 15) ? 255 : 0;
          break;
        }
        case 4: {
          const uint16_t v = Le16(b);
          natural[0][0] = Expand(v & 31, 5);
          natural[0][1] = Expand((v >> 5) & 63, 6);
          natural[0][2] = natural[0][3] = Expand((v >> 11) & 31, 5);
          break;
        }
        case 6: {
          const uint32_t v = Le32(b);
          for (int c = 0; c < 4; ++c) natural[0][c] = uint8_t(v >> (8 * c));
          break;
        }
        case 7: {
          const uint32_t v = Le32(b);
          natural[0][0] = uint8_t((v & 1023) >> 2);
          natural[0][1] = uint8_t(((v >> 10) & 1023) >> 2);
          natural[0][2] = uint8_t(((v >> 20) & 1023) >> 2);
          natural[0][3] = Expand(v >> 30, 2);
          break;
        }
        case 10: {
          const uint16_t v = Le16(b);
          natural[0][0] = uint8_t(v);
          natural[0][1] = natural[0][2] = natural[0][3] = uint8_t(v >> 8);
          break;
        }
        case 15: {
          const uint16_t v = Le16(b);
          for (int c = 0; c < 4; ++c) natural[0][c] = uint8_t(((v >> (4 * c)) & 15) * 17);
          break;
        }
        case 18:
          DecodeDxtColor(b, true, natural);
          break;
        case 19:
        case 20: {
          DecodeDxtColor(b + 8, false, natural);
          uint8_t a[16];
          if (format == 19) {
            DecodeDxt3Alpha(b, a);
          } else {
            DecodeDxt5Alpha(b, a);
          }
          for (int i = 0; i < 16; ++i) natural[i][3] = a[i];
          break;
        }
        case 49: {
          uint8_t x[16], y[16];
          DecodeDxt5Alpha(b, x);
          DecodeDxt5Alpha(b + 8, y);
          for (int i = 0; i < 16; ++i) {
            natural[i][0] = x[i];
            natural[i][1] = natural[i][2] = natural[i][3] = y[i];
          }
          break;
        }
        case 58:
        case 59: {
          uint8_t a[16];
          if (format == 58) {
            DecodeDxt3Alpha(b, a);
          } else {
            DecodeDxt5Alpha(b, a);
          }
          for (int i = 0; i < 16; ++i) natural[i][0] = natural[i][1] = natural[i][2] = natural[i][3] = a[i];
          break;
        }
      }
      // Store with the fetch swizzle.
      const uint32_t swizzle = fetch.swizzle();
      for (uint32_t ty = 0; ty < bh; ++ty) {
        for (uint32_t tx = 0; tx < bw; ++tx) {
          const uint32_t px = bx * bw + tx, py = by * bh + ty;
          if (px >= L.width || py >= L.height) continue;
          const uint8_t* n = natural[ty * bw + tx];
          uint8_t* o = &rgba[(size_t(py) * L.width + px) * 4];
          for (int c = 0; c < 4; ++c) {
            const uint32_t s = (swizzle >> (3 * c)) & 7;
            o[c] = s < 4 ? n[s] : (s == 5 ? 255 : 0);
          }
        }
      }
    }
  }
}

// One level's blocks, untiled, as they are (endian-swapped data).
void CopyLevelBlocks(const TextureFetch& fetch, uint32_t bw, uint32_t bh, uint32_t bpb,
                     const uint8_t* buf, size_t size, const LevelPlacement& L, uint8_t* out) {
  const bool tiled = fetch.tiled();
  const uint32_t wb = (L.width + bw - 1) / bw;
  const uint32_t hb = (L.height + bh - 1) / bh;
  const uint32_t log2_bpb = Log2(bpb);
  for (uint32_t by = 0; by < hb; ++by) {
    uint8_t* row = out + size_t(by) * wb * bpb;
    if (!tiled) {
      // Linear: whole rows at once.
      const size_t offset =
          L.offset_bytes + size_t(by + L.off_by) * L.row_bytes + size_t(L.off_bx) * bpb;
      if (offset + size_t(wb) * bpb <= size) {
        std::memcpy(row, buf + offset, size_t(wb) * bpb);
        continue;
      }
    }
    for (uint32_t bx = 0; bx < wb; ++bx) {
      const size_t offset =
          L.offset_bytes +
          (tiled ? TiledOffset(bx + L.off_bx, by + L.off_by, L.pitch_blocks, log2_bpb)
                 : size_t(by + L.off_by) * L.row_bytes + size_t(bx + L.off_bx) * bpb);
      if (offset + bpb > size) {
        std::memset(row + size_t(bx) * bpb, 0, bpb);
      } else {
        std::memcpy(row + size_t(bx) * bpb, buf + offset, bpb);
      }
    }
  }
}

}  // namespace

uint32_t BcFormatFor(uint32_t format) {
  switch (format) {
    case 18: return 1;  // k_DXT1
    case 19: return 2;  // k_DXT2_3
    case 20: return 3;  // k_DXT4_5
    case 59: return 4;  // k_DXT5A
    case 49: return 5;  // k_DXN
    default: return 0;
  }
}

uint32_t BcChannel(uint32_t bc, uint32_t natural) {
  switch (bc) {
    case 4: return 0;                          // DXT5A: a in every channel
    case 5: return natural == 0 ? 0 : 1;       // DXN: x, then y in Y, Z and W
    default: return natural;                   // DXT1..5: RGBA
  }
}

const char* TextureFormatName(uint32_t format) {
  switch (format) {
    case 2: return "k_8";
    case 5: return "k_6_5_5";
    case 8: return "k_8_A";
    case 9: return "k_8_B";
    case 11: return "k_Cr_Y1_Cb_Y0_REP";
    case 12: return "k_Y1_Cr_Y0_Cb_REP";
    case 14: return "k_8_8_8_8_A";
    case 16: return "k_10_11_11";
    case 17: return "k_11_11_10";
    case 24: return "k_16";
    case 25: return "k_16_16";
    case 26: return "k_16_16_16_16";
    case 30: return "k_16_FLOAT";
    case 31: return "k_16_16_FLOAT";
    case 32: return "k_16_16_16_16_FLOAT";
    case 36: return "k_32_FLOAT";
    case 37: return "k_32_32_FLOAT";
    case 38: return "k_32_32_32_32_FLOAT";
    case 3: return "k_1_5_5_5";
    case 4: return "k_5_6_5";
    case 6: return "k_8_8_8_8";
    case 7: return "k_2_10_10_10";
    case 10: return "k_8_8";
    case 15: return "k_4_4_4_4";
    case 18: return "k_DXT1";
    case 19: return "k_DXT2_3";
    case 20: return "k_DXT4_5";
    case 23: return "k_24_8_FLOAT";
    case 49: return "k_DXN";
    case 58: return "k_DXT3A";
    case 59: return "k_DXT5A";
    default: return "unknown";
  }
}

bool IsDecodableTextureFormat(uint32_t format) {
  uint32_t bw, bh, bpb;
  return format != 23 && GetFormatInfo(format, &bw, &bh, &bpb);
}

size_t TextureBaseSize(const TextureFetch& fetch) {
  uint32_t bw, bh, bpb;
  if (!GetFormatInfo(fetch.format(), &bw, &bh, &bpb)) return 0;
  const uint32_t wb = (std::max(fetch.pitch_texels(), fetch.width()) + bw - 1) / bw;
  const uint32_t hb = (fetch.height() + bh - 1) / bh;
  const uint32_t pitch_b = (wb + 31) & ~31u;
  const uint32_t rows = (hb + 31) & ~31u;
  return (size_t(pitch_b) * rows * bpb + 4095) & ~size_t(4095);
}

DecodedTexture DecodeTexture(const TextureFetch& fetch, const uint8_t* data, size_t size) {
  DecodedTexture out;
  out.width = fetch.width();
  out.height = fetch.dimension() == 0 ? 1 : fetch.height();
  const size_t texels = size_t(out.width) * out.height;
  out.rgba.assign(texels * 4, 0);
  auto placeholder = [&](const std::string& why) {
    for (size_t i = 0; i < texels; ++i) {
      out.rgba[i * 4 + 0] = 255;
      out.rgba[i * 4 + 1] = 0;
      out.rgba[i * 4 + 2] = 255;
      out.rgba[i * 4 + 3] = 255;
    }
    out.note = why;
    return out;
  };
  const uint32_t format = fetch.format();
  uint32_t bw, bh, bpb;
  if (!IsDecodableTextureFormat(format) || !GetFormatInfo(format, &bw, &bh, &bpb)) {
    return placeholder(std::string("format ") + std::to_string(format) + " (" +
                       TextureFormatName(format) + ") not decoded");
  }
  if (fetch.dimension() > 1) return placeholder("3D / cube textures not decoded yet");
  if (!data || !size) return placeholder("no data");

  std::vector<uint8_t> buf(data, data + size);
  Swap(buf.data(), buf.size(), fetch.endian());

  const LevelPlacement L = PlaceLevel(fetch, 0, bw, bh, bpb);
  DecodeLevel(fetch, format, bw, bh, bpb, buf.data(), buf.size(), L, out.rgba.data());
  return out;
}

std::vector<DecodedTexture> DecodeTextureMips(const TextureFetch& fetch, const uint8_t* base,
                                              size_t base_size, const uint8_t* mips,
                                              size_t mips_size, bool keep_blocks) {
  std::vector<DecodedTexture> levels;
  uint32_t bw, bh, bpb;
  const uint32_t bc = keep_blocks ? BcFormatFor(fetch.format()) : 0;
  if (bc && fetch.dimension() <= 1 && base && base_size &&
      GetFormatInfo(fetch.format(), &bw, &bh, &bpb)) {
    // The blocks as they are, for the GPU to decode.
    auto level_blocks = [&](const uint8_t* data, size_t size, uint32_t level) {
      const LevelPlacement L = PlaceLevel(fetch, level, bw, bh, bpb);
      DecodedTexture d;
      d.width = L.width;
      d.height = L.height;
      d.bc = bc;
      d.rgba.resize(size_t((L.width + bw - 1) / bw) * ((L.height + bh - 1) / bh) * bpb);
      CopyLevelBlocks(fetch, bw, bh, bpb, data, size, L, d.rgba.data());
      return d;
    };
    {
      std::vector<uint8_t> buf(base, base + base_size);
      Swap(buf.data(), buf.size(), fetch.endian());
      levels.push_back(level_blocks(buf.data(), buf.size(), 0));
    }
    if (!mips || !mips_size || fetch.mip_address() == 0) return levels;
    const uint32_t w = fetch.width(), h = fetch.dimension() == 0 ? 1 : fetch.height();
    const uint32_t last = std::min(fetch.mip_max_level(), Log2(std::max(w, h)));
    std::vector<uint8_t> buf(mips, mips + mips_size);
    Swap(buf.data(), buf.size(), fetch.endian());
    for (uint32_t level = 1; level <= last; ++level) {
      const LevelPlacement L = PlaceLevel(fetch, level, bw, bh, bpb);
      size_t end = 0;
      const uint32_t wb = (L.width + bw - 1) / bw, hb = (L.height + bh - 1) / bh;
      if (fetch.tiled()) {
        for (uint32_t by = 0; by < hb; ++by) {
          for (uint32_t bx = 0; bx < wb; ++bx) {
            end = std::max(end, L.offset_bytes +
                                    TiledOffset(bx + L.off_bx, by + L.off_by, L.pitch_blocks,
                                                Log2(bpb)) +
                                    bpb);
          }
        }
      } else {
        end = L.offset_bytes + size_t(hb - 1 + L.off_by) * L.row_bytes +
              size_t(wb + L.off_bx) * bpb;
      }
      if (end > buf.size()) break;
      levels.push_back(level_blocks(buf.data(), buf.size(), level));
    }
    return levels;
  }
  levels.push_back(DecodeTexture(fetch, base, base_size));
  if (!levels[0].note.empty() || !mips || !mips_size || fetch.dimension() > 1 ||
      fetch.mip_address() == 0 || !GetFormatInfo(fetch.format(), &bw, &bh, &bpb)) {
    return levels;
  }
  const uint32_t w = fetch.width(), h = fetch.dimension() == 0 ? 1 : fetch.height();
  const uint32_t last = std::min(fetch.mip_max_level(), Log2(std::max(w, h)));
  std::vector<uint8_t> buf(mips, mips + mips_size);
  Swap(buf.data(), buf.size(), fetch.endian());
  for (uint32_t level = 1; level <= last; ++level) {
    const LevelPlacement L = PlaceLevel(fetch, level, bw, bh, bpb);
    // Stop at the first level that isn't in the data (older captures saved
    // too little of the mip chain); the sampler then clamps to what's here.
    size_t end = 0;
    const uint32_t wb = (L.width + bw - 1) / bw, hb = (L.height + bh - 1) / bh;
    for (uint32_t by = 0; by < hb; ++by) {
      for (uint32_t bx = 0; bx < wb; ++bx) {
        const size_t o =
            fetch.tiled() ? TiledOffset(bx + L.off_bx, by + L.off_by, L.pitch_blocks, Log2(bpb))
                          : size_t(by + L.off_by) * L.row_bytes + size_t(bx + L.off_bx) * bpb;
        end = std::max(end, L.offset_bytes + o + bpb);
      }
    }
    if (end > buf.size()) break;
    DecodedTexture d;
    d.width = L.width;
    d.height = L.height;
    d.rgba.assign(size_t(d.width) * d.height * 4, 0);
    DecodeLevel(fetch, fetch.format(), bw, bh, bpb, buf.data(), buf.size(), L, d.rgba.data());
    levels.push_back(std::move(d));
  }
  return levels;
}

}  // namespace amped3::native

