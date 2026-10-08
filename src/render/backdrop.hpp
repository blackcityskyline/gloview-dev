#pragma once

#include <hyprland/src/render/Framebuffer.hpp>

namespace gloview::render {

struct BlurCache {
  SP<Render::IFramebuffer> fb;
  const void *srcId = nullptr;
  // recipe is part of the key, checked every frame
  int passes = 0, sizePx = 0, resolution = 0;
  float strength = -1.0F;
  bool valid = false;
  [[nodiscard]] bool matches(const void *id, int p, int s, int r,
                             float st) const {
    return valid && srcId == id && passes == p && sizePx == s &&
           resolution == r && strength == st;
  }
  void invalidate() { valid = false; }
  void drop() {
    invalidate();
    fb.reset();
  }
};

}
