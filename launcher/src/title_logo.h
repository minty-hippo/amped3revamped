// The launcher's title: an animated GIF (assets/title.gif, built into the
// program) that plays once and then holds its last frame.

#pragma once

#include <memory>

struct SDL_Renderer;
struct SDL_Texture;

namespace launcher {

class TitleLogo {
 public:
  explicit TitleLogo(SDL_Renderer* renderer);
  ~TitleLogo();
  TitleLogo(const TitleLogo&) = delete;
  TitleLogo& operator=(const TitleLogo&) = delete;

  // Advances the animation to now and returns the frame to draw, made for
  // about `height` pixels on screen (null: the GIF couldn't be read).
  SDL_Texture* Frame(float height);
  // The picture's aspect (width / height), its transparent margin left out,
  // and that part's texture coordinates.
  float Aspect() const;
  void Uv(float* u0, float* v0, float* u1, float* v1) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace launcher
