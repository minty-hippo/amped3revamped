// The launcher's title animation (title_logo.h).
//
// Frames are decoded one at a time as they come due, with stb_image's GIF
// reader (its per-frame step: the whole animation decoded at once is 84
// frames of 1482 x 682, over 300 MB), shrunk on the CPU to about the size
// drawn (a filtered texture shrunk 3-4x on the GPU flickers along the thin
// outlines), and uploaded to one streaming texture.

#include "title_logo.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <SDL3/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// The GIF's bytes (generated from assets/title.gif by CMakeLists.txt).
extern "C" const unsigned char kTitleGif[];
extern "C" const unsigned int kTitleGifSize;

namespace launcher {

namespace {
// The part of the picture anything is drawn in, over the whole animation (the
// rest is transparent), in the GIF's pixels: x0, y0, x1, y1.
constexpr int kCrop[4] = {37, 31, 1457, 665};
}  // namespace

struct TitleLogo::Impl {
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;
  int tex_w = 0, tex_h = 0, factor = 0;
  stbi__context ctx{};
  stbi__gif gif{};
  bool ok = false, started = false, done = false;
  uint64_t next_ms = 0;  // when the next frame is due
  std::vector<uint8_t> shrunk;

  ~Impl() {
    if (texture) SDL_DestroyTexture(texture);
    STBI_FREE(gif.out);
    STBI_FREE(gif.history);
    STBI_FREE(gif.background);
  }

  // The current frame (gif.out), shrunk by `factor` and uploaded.
  void Upload() {
    const int w = gif.w, h = gif.h;
    const int ow = std::max(w / factor, 1), oh = std::max(h / factor, 1);
    if (!texture || tex_w != ow || tex_h != oh) {
      if (texture) SDL_DestroyTexture(texture);
      texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                  ow, oh);
      if (!texture) return;
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
      SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
      tex_w = ow;
      tex_h = oh;
    }
    const uint8_t* src = gif.out;
    if (factor == 1) {
      SDL_UpdateTexture(texture, nullptr, src, w * 4);
      return;
    }
    // Box filter, colors weighted by alpha (no dark fringe at the edges).
    shrunk.resize(size_t(ow) * oh * 4);
    for (int y = 0; y < oh; ++y) {
      for (int x = 0; x < ow; ++x) {
        uint32_t r = 0, g = 0, b = 0, a = 0;
        for (int dy = 0; dy < factor; ++dy) {
          const uint8_t* p = src + (size_t(y * factor + dy) * w + size_t(x) * factor) * 4;
          for (int dx = 0; dx < factor; ++dx, p += 4) {
            r += p[0] * p[3];
            g += p[1] * p[3];
            b += p[2] * p[3];
            a += p[3];
          }
        }
        uint8_t* o = &shrunk[(size_t(y) * ow + x) * 4];
        o[0] = uint8_t(a ? r / a : 0);
        o[1] = uint8_t(a ? g / a : 0);
        o[2] = uint8_t(a ? b / a : 0);
        o[3] = uint8_t(a / uint32_t(factor * factor));
      }
    }
    SDL_UpdateTexture(texture, nullptr, shrunk.data(), ow * 4);
  }

  // Decodes the next frame; false at the end (the last one stays).
  bool Next() {
    int comp = 0;
    stbi_uc* u = stbi__gif_load_next(&ctx, &gif, &comp, 4, nullptr);
    if (!u || u == reinterpret_cast<stbi_uc*>(&ctx)) return false;
    // GIF delays are in hundredths of a second (stb gives milliseconds); 0
    // usually means "as fast as possible", which browsers show as 100 ms.
    next_ms += uint64_t(gif.delay > 10 ? gif.delay : 100);
    return true;
  }
};

TitleLogo::TitleLogo(SDL_Renderer* renderer) : impl_(std::make_unique<Impl>()) {
  impl_->renderer = renderer;
  stbi__start_mem(&impl_->ctx, kTitleGif, int(kTitleGifSize));
  impl_->ok = stbi__gif_test(&impl_->ctx) != 0;
}

TitleLogo::~TitleLogo() = default;

SDL_Texture* TitleLogo::Frame(float height) {
  Impl& m = *impl_;
  if (!m.ok) return nullptr;
  // Shrunk by a whole factor to no smaller than drawn (the GPU's filter does
  // the rest, under 2x).
  const float crop_h = float(kCrop[3] - kCrop[1]);
  const int factor = std::clamp(int(crop_h / std::max(height, 1.0f)), 1, 8);
  const uint64_t now = SDL_GetTicks();
  bool changed = false;
  if (!m.started) {
    m.started = true;
    m.next_ms = now;
    if (!m.Next()) {
      m.ok = false;
      return nullptr;
    }
    changed = true;
  }
  // Frames due by now (a slow frame skips ahead rather than slowing down).
  while (!m.done && now >= m.next_ms) {
    if (!m.Next()) {
      m.done = true;
      break;
    }
    changed = true;
  }
  if (factor != m.factor) {
    m.factor = factor;
    changed = true;
  }
  if (changed) m.Upload();
  return m.texture;
}

float TitleLogo::Aspect() const {
  return float(kCrop[2] - kCrop[0]) / float(kCrop[3] - kCrop[1]);
}

void TitleLogo::Uv(float* u0, float* v0, float* u1, float* v1) const {
  const float w = impl_->gif.w > 0 ? float(impl_->gif.w) : 1482.0f;
  const float h = impl_->gif.h > 0 ? float(impl_->gif.h) : 682.0f;
  *u0 = float(kCrop[0]) / w;
  *v0 = float(kCrop[1]) / h;
  *u1 = float(kCrop[2]) / w;
  *v1 = float(kCrop[3]) / h;
}

}  // namespace launcher
