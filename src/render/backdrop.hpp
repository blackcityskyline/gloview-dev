#pragma once

// The Pixels store's backdrop cache (REFACTORING.md M1): the blurred-wallpaper
// FBO plus the identity of what it was blurred FROM. Owned by the session,
// mutated only by backdrop.cpp at explicit capture moments.

#include <hyprland/src/render/Framebuffer.hpp>

namespace gloview::render {

struct BlurCache {
  SP<Render::IFramebuffer> fb;
  const void *srcId = nullptr;
  // The filter recipe the fb was rendered with. Part of the cache KEY
  // (compared every frame together with srcId): without it, changing
  // blur_passes/blur_size/blur_resolution/blur_strength mid-session would
  // keep showing a texture baked with the old params until reopen.
  int passes = 0, sizePx = 0, resolution = 0;
  float strength = -1.0F;
  bool valid = false;
  [[nodiscard]] bool matches(const void *id, int p, int s, int r,
                             float st) const {
    return valid && srcId == id && passes == p && sizePx == s &&
           resolution == r && strength == st;
  }
  void invalidate() { valid = false; }
  void drop() { // full teardown: free the FBO too (open / unload)
    invalidate();
    fb.reset();
  }
};

} // namespace gloview::render
