// Where a texture's data is in guest memory: the base level and the mip
// chain, as physical address ranges (upper bounds of what the GPU reads).
// Shared by the frame capture and the live native renderer.

#pragma once

#include <cstdint>

namespace amped3 {

struct TextureRanges {
  uint32_t base_address = 0, base_size = 0;  // physical; size 0 = none
  uint32_t mip_address = 0, mip_size = 0;
  uint32_t width = 0, height = 0, depth = 0;
  uint32_t mip_min = 0, mip_max = 0;
};

// `fetch`: the six fetch constant dwords (host order).
TextureRanges GetTextureRanges(const uint32_t fetch[6]);

}  // namespace amped3
