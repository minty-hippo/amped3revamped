// Xenos texture fetch constants and CPU texture decoding for the native
// renderer (step 2 / step 4). The C++ counterpart of tools/capture/texdecode.py:
// endian swap, 2D untiling, format decode to RGBA8, fetch-constant swizzle.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace amped3::native {

// The six dwords of a texture fetch constant, decoded (xenos.h
// xe_gpu_texture_fetch_t).
struct TextureFetch {
  uint32_t words[6] = {};
  uint32_t type() const { return words[0] & 3; }
  uint32_t clamp(int axis) const { return (words[0] >> (10 + 3 * axis)) & 7; }
  uint32_t pitch_texels() const { return ((words[0] >> 22) & 0x1FF) * 32; }
  bool tiled() const { return (words[0] >> 31) & 1; }
  uint32_t format() const { return words[1] & 63; }
  uint32_t endian() const { return (words[1] >> 6) & 3; }
  uint32_t base_address() const { return words[1] & 0xFFFFF000; }  // physical
  uint32_t width() const { return (words[2] & 0x1FFF) + 1; }        // 2D
  uint32_t height() const { return ((words[2] >> 13) & 0x1FFF) + 1; }
  uint32_t swizzle() const { return (words[3] >> 1) & 0xFFF; }
  uint32_t mag_filter() const { return (words[3] >> 19) & 3; }
  uint32_t min_filter() const { return (words[3] >> 21) & 3; }
  uint32_t mip_filter() const { return (words[3] >> 23) & 3; }
  uint32_t aniso_filter() const { return (words[3] >> 25) & 7; }
  uint32_t mip_min_level() const { return (words[4] >> 2) & 15; }
  uint32_t mip_max_level() const { return (words[4] >> 6) & 15; }
  uint32_t dimension() const { return (words[5] >> 9) & 3; }  // 0 1D, 1 2D, 2 3D, 3 cube
  uint32_t mip_address() const { return words[5] & 0xFFFFF000; }
  bool packed_mips() const { return (words[5] >> 11) & 1; }
  uint32_t border_color() const { return words[5] & 3; }
};

struct DecodedTexture {
  uint32_t width = 0, height = 0;
  // width * height * 4 RGBA8, top row first; or, when `bc` is set, the
  // untiled compressed blocks (row-major, ceil(w/4) x ceil(h/4) blocks).
  std::vector<uint8_t> rgba;
  std::string note;  // why it is a placeholder, if it is one
  // Compressed blocks kept as they are (DecodeTextureMips with keep_blocks):
  // 1..5 = BC1..BC5. Their channels are the format's natural XYZW mapped as
  // BcChannel says, with the fetch swizzle still to apply (when sampling).
  uint32_t bc = 0;
};

// The BC format the guest format's blocks are, if any (0: none): DXT1 = BC1,
// DXT2_3 = BC2, DXT4_5 = BC3, DXT5A = BC4, DXN = BC5.
uint32_t BcFormatFor(uint32_t format);
// Which channel of the BC image (0 R, 1 G, 2 B, 3 A) holds natural channel
// `natural` (0..3) of a `bc` texture.
uint32_t BcChannel(uint32_t bc, uint32_t natural);

// Whether DecodeTexture understands the format.
bool IsDecodableTextureFormat(uint32_t format);
const char* TextureFormatName(uint32_t format);

// Decodes the base level of a 2D texture from its raw guest data (as read at
// the base address, still tiled and big-endian) to RGBA8 with the fetch
// constant's swizzle applied. Unknown formats give a magenta placeholder of the
// right size and set `note`.
DecodedTexture DecodeTexture(const TextureFetch& fetch, const uint8_t* data, size_t size);

// The base level and the mip chain (levels 1..mip_max_level from the mip
// address data), each decoded like DecodeTexture. Only the base level when
// there is no mip data.
// keep_blocks: block-compressed formats with a BC equivalent come back as
// their blocks (`bc` set), for the GPU to decode; the fetch swizzle is then
// not applied.
std::vector<DecodedTexture> DecodeTextureMips(const TextureFetch& fetch, const uint8_t* base,
                                              size_t base_size, const uint8_t* mips,
                                              size_t mips_size, bool keep_blocks = false);

// Byte size the base level occupies in guest memory (upper bound, including
// tiling alignment).
size_t TextureBaseSize(const TextureFetch& fetch);

}  // namespace amped3::native
