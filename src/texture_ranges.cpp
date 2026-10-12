#include "texture_ranges.h"

#include <algorithm>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace amped3 {
namespace {

// Block size per Xenos texture format (copied from the SDK's info_formats.cpp).
struct FormatBlock {
  uint32_t width, height, bits_per_pixel;
};
constexpr FormatBlock kFormatBlocks[64] = {
    {1, 1, 1}  /* 0 k_1_REVERSE */,
    {1, 1, 1}  /* 1 k_1 */,
    {1, 1, 8}  /* 2 k_8 */,
    {1, 1, 16}  /* 3 k_1_5_5_5 */,
    {1, 1, 16}  /* 4 k_5_6_5 */,
    {1, 1, 16}  /* 5 k_6_5_5 */,
    {1, 1, 32}  /* 6 k_8_8_8_8 */,
    {1, 1, 32}  /* 7 k_2_10_10_10 */,
    {1, 1, 8}  /* 8 k_8_A */,
    {1, 1, 8}  /* 9 k_8_B */,
    {1, 1, 16}  /* 10 k_8_8 */,
    {2, 1, 16}  /* 11 k_Cr_Y1_Cb_Y0_REP */,
    {2, 1, 16}  /* 12 k_Y1_Cr_Y0_Cb_REP */,
    {1, 1, 32}  /* 13 k_16_16_EDRAM */,
    {1, 1, 32}  /* 14 k_8_8_8_8_A */,
    {1, 1, 16}  /* 15 k_4_4_4_4 */,
    {1, 1, 32}  /* 16 k_10_11_11 */,
    {1, 1, 32}  /* 17 k_11_11_10 */,
    {4, 4, 4}  /* 18 k_DXT1 */,
    {4, 4, 8}  /* 19 k_DXT2_3 */,
    {4, 4, 8}  /* 20 k_DXT4_5 */,
    {1, 1, 64}  /* 21 k_16_16_16_16_EDRAM */,
    {1, 1, 32}  /* 22 k_24_8 */,
    {1, 1, 32}  /* 23 k_24_8_FLOAT */,
    {1, 1, 16}  /* 24 k_16 */,
    {1, 1, 32}  /* 25 k_16_16 */,
    {1, 1, 64}  /* 26 k_16_16_16_16 */,
    {1, 1, 16}  /* 27 k_16_EXPAND */,
    {1, 1, 32}  /* 28 k_16_16_EXPAND */,
    {1, 1, 64}  /* 29 k_16_16_16_16_EXPAND */,
    {1, 1, 16}  /* 30 k_16_FLOAT */,
    {1, 1, 32}  /* 31 k_16_16_FLOAT */,
    {1, 1, 64}  /* 32 k_16_16_16_16_FLOAT */,
    {1, 1, 32}  /* 33 k_32 */,
    {1, 1, 64}  /* 34 k_32_32 */,
    {1, 1, 128}  /* 35 k_32_32_32_32 */,
    {1, 1, 32}  /* 36 k_32_FLOAT */,
    {1, 1, 64}  /* 37 k_32_32_FLOAT */,
    {1, 1, 128}  /* 38 k_32_32_32_32_FLOAT */,
    {4, 1, 8}  /* 39 k_32_AS_8 */,
    {2, 1, 16}  /* 40 k_32_AS_8_8 */,
    {1, 1, 16}  /* 41 k_16_MPEG */,
    {1, 1, 32}  /* 42 k_16_16_MPEG */,
    {1, 1, 8}  /* 43 k_8_INTERLACED */,
    {4, 1, 8}  /* 44 k_32_AS_8_INTERLACED */,
    {1, 1, 16}  /* 45 k_32_AS_8_8_INTERLACED */,
    {1, 1, 16}  /* 46 k_16_INTERLACED */,
    {1, 1, 16}  /* 47 k_16_MPEG_INTERLACED */,
    {1, 1, 32}  /* 48 k_16_16_MPEG_INTERLACED */,
    {4, 4, 8}  /* 49 k_DXN */,
    {1, 1, 32}  /* 50 k_8_8_8_8_AS_16_16_16_16 */,
    {4, 4, 4}  /* 51 k_DXT1_AS_16_16_16_16 */,
    {4, 4, 8}  /* 52 k_DXT2_3_AS_16_16_16_16 */,
    {4, 4, 8}  /* 53 k_DXT4_5_AS_16_16_16_16 */,
    {1, 1, 32}  /* 54 k_2_10_10_10_AS_16_16_16_16 */,
    {1, 1, 32}  /* 55 k_10_11_11_AS_16_16_16_16 */,
    {1, 1, 32}  /* 56 k_11_11_10_AS_16_16_16_16 */,
    {1, 1, 96}  /* 57 k_32_32_32_FLOAT */,
    {4, 4, 4}  /* 58 k_DXT3A */,
    {4, 4, 4}  /* 59 k_DXT5A */,
    {4, 4, 4}  /* 60 k_CTX1 */,
    {4, 4, 4}  /* 61 k_DXT3A_AS_1_1_1_1 */,
    {1, 1, 32}  /* 62 k_8_8_8_8_GAMMA_EDRAM */,
    {1, 1, 32}  /* 63 k_2_10_10_10_FLOAT_EDRAM */};

}  // namespace

TextureRanges GetTextureRanges(const uint32_t d[6]) {
  TextureRanges out;
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
  fetch.dword_0 = d[0];
  fetch.dword_1 = d[1];
  fetch.dword_2 = d[2];
  fetch.dword_3 = d[3];
  fetch.dword_4 = d[4];
  fetch.dword_5 = d[5];
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  rex::graphics::texture_util::GetSubresourcesFromFetchConstant(
      fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page, &mip_page, &mip_min,
      &mip_max);
  out.width = width_minus_1 + 1;
  out.height = height_minus_1 + 1;
  out.depth = depth_minus_1 + 1;
  out.mip_min = mip_min;
  out.mip_max = mip_max;
  // Base level: rows and pitch aligned to 32x32-block tiles when tiled,
  // slices to 4 KB.
  const FormatBlock& block = kFormatBlocks[uint32_t(fetch.format) & 63];
  const uint32_t pitch_texels = std::max<uint32_t>(fetch.pitch * 32, width_minus_1 + 1);
  uint32_t pitch_blocks = (pitch_texels + block.width - 1) / block.width;
  uint32_t rows_blocks = (height_minus_1 + 1 + block.height - 1) / block.height;
  if (fetch.tiled) {
    pitch_blocks = (pitch_blocks + 31) & ~31u;
    rows_blocks = (rows_blocks + 31) & ~31u;
  }
  uint64_t slice_bytes = uint64_t(pitch_blocks) * rows_blocks * block.bits_per_pixel *
                         block.width * block.height / 8;
  slice_bytes = (slice_bytes + 4095) & ~uint64_t(4095);
  const uint64_t base_bytes = slice_bytes * (depth_minus_1 + 1);
  auto clamp_to_physical = [](uint32_t page, uint64_t bytes) -> uint32_t {
    const uint64_t start = uint64_t(page) << 12;
    const uint64_t end = std::min<uint64_t>(start + bytes, 0x20000000ull);
    return end > start ? uint32_t(std::min<uint64_t>(end - start, 64ull << 20)) : 0;
  };
  if (base_page) {
    out.base_address = base_page << 12;
    out.base_size = clamp_to_physical(base_page, base_bytes);
  }
  // The mip chain's real extent: mips are laid out for a power-of-two base and
  // tile aligned, so for small textures they take more than the base level.
  if (mip_page && mip_max > 0) {
    const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, width_minus_1 + 1, height_minus_1 + 1, depth_minus_1 + 1,
        fetch.tiled, fetch.format, fetch.packed_mips, false, mip_max);
    const uint64_t mip_bytes = std::max<uint64_t>(
        base_bytes, (uint64_t(layout.mips_total_extent_bytes) + 4095) & ~uint64_t(4095));
    out.mip_address = mip_page << 12;
    out.mip_size = clamp_to_physical(mip_page, mip_bytes);
  }
  return out;
}

}  // namespace amped3
