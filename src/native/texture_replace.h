// amped3 - texture replacements for the native renderer.
//
// A game texture can be swapped for a bigger one with the same layout: the
// game's texture coordinates are fractions of the texture, so a 4x image
// drops in. Two sources, looked up by a hash of the texture's guest data:
//
//   - The texture pack: <folder>/*.png named <anything>_<hash>.png (or
//     <hash>.png), the hash as 16 hex digits (see ContentHash). Shipped
//     replacements (the new logos) live here, and anyone can add more.
//   - The upscaler's output (<folder>/upscaled, made on the player's PC at
//     install by tools/texture-upscale): only where neither the pack nor
//     the fonts replace a texture.
//   - Sharper fonts: the game's five bitmap fonts, rebuilt at 4x from their
//     own glyphs when first used (UpscaleGlyphAtlas): each glyph's outline is
//     traced to a vector shape (straight edges, sharp corners, curves) and
//     filled anti-aliased at the new size. Kept in the cache folder for later
//     sessions; nothing derived from the game is shipped.
//
// Block-compressed (DXT) 2D textures only, which is what the game's UI uses.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenos_texture.h"

namespace amped3::native {

struct ReplacementLevel {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4, top row first
};

struct Replacement {
  // The full mip chain, largest first (down to 1x1).
  std::vector<ReplacementLevel> levels;
  // The texels are as sampled (the fetch constant's swizzle already applied,
  // as the CPU decoder does); else in the format's natural order (a texture
  // pack PNG), the swizzle still to apply when sampling.
  bool swizzled = false;
  std::string source;  // for the log
};

class TextureReplacer {
 public:
  // folder: the texture pack ("" or missing: none); hd_fonts: rebuild the
  // game's fonts at 4x, kept (compressed) in cache_folder so later sessions
  // load them instead ("": rebuilt each session).
  // upscaled: also the upscaler's output, <folder>/upscaled (used only for
  // textures the pack and the fonts don't replace).
  TextureReplacer(const std::string& folder, bool texture_pack, bool hd_fonts,
                  const std::string& cache_folder = {}, bool upscaled = false);

  // Whether a texture with this fetch constant could be replaced (cheap,
  // before hashing its data).
  bool MayReplace(const TextureFetch& tf) const;

  // FNV-1a over the base level's blocks as the guest has them (tiled,
  // big-endian): ceil(w/4) * ceil(h/4) blocks of 8 (DXT1, DXT5A) or 16 bytes.
  // 0 if `size` is short of that or the format isn't block-compressed.
  static uint64_t ContentHash(const TextureFetch& tf, const uint8_t* data, size_t size);

  // The replacement for a texture (base level data as the guest has it), or
  // null. Loads or builds it the first time (can take a few hundred ms).
  std::shared_ptr<const Replacement> Find(const TextureFetch& tf, const uint8_t* data,
                                          size_t size);

  size_t pack_size() const { return pack_.size(); }
  size_t upscaled_size() const { return upscaled_.size(); }

  // The upscaler's subfolder of the pack folder (tools/texture-upscale).
  static constexpr const char* kUpscaledFolder = "upscaled";

  // The game's fonts (rebuilt by hd_fonts; the upscaler leaves them alone).
  static bool IsFont(uint64_t hash);
  // What happened since the last call (loads, failures), one line each.
  std::vector<std::string> TakeMessages();

 private:
  std::unordered_map<uint64_t, std::string> pack_;  // hash -> PNG path
  std::unordered_map<uint64_t, std::string> upscaled_;
  bool hd_fonts_ = false;
  std::string cache_folder_;
  std::mutex mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<const Replacement>> loaded_;
  std::unordered_map<uint64_t, bool> failed_;
  std::vector<std::string> messages_;
};

// Rebuilds a glyph atlas (straight-alpha RGBA, w x h) at `scale` times its
// size: the opaque shape, the light shape within it (white text in its black
// outline) and the rim band are traced as vector outlines and filled with
// exact anti-aliasing; shaded or coloured areas (button icons) take their
// colour interpolated from the original.
std::vector<uint8_t> UpscaleGlyphAtlas(const uint8_t* rgba, uint32_t w, uint32_t h,
                                       uint32_t scale);

// The mip chain of an RGBA image, alpha-weighted 2x2 box filtering, down to 1x1.
std::vector<ReplacementLevel> BuildMips(ReplacementLevel base);

}  // namespace amped3::native
