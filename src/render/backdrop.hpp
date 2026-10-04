#pragma once

// Backdrop cache: the blurred-wallpaper FBO plus what it was blurred FROM and
// with which recipe. Mutated only at explicit capture moments in backdrop.cpp.

#include <hyprland/src/render/Framebuffer.hpp>

namespace gloview::render {

struct BlurCache {
  SP<Render::IFramebuffer> fb;
  const void *srcId = nullptr;
  // Recipe is part of the cache KEY (checked with srcId every frame): without
  // it a mid-session blur_* change keeps showing a texture baked with the old
  // params until reopen.
  int passes = 0, sizePx = 0, resolution = 0;
  float strength = -1.0F;
  bool valid = false;
  [[nodiscard]] bool matches(const void *id, int p, int s, int r,
                             float st) const {
    return valid && srcId == id && passes == p && sizePx == s &&
           resolution == r && strength == st;
  }
  void invalidate() { valid = false; }
  void drop() { // full teardown, FBO included (open / unload)
    invalidate();
    fb.reset();
  }
};

} // namespace gloview::render
