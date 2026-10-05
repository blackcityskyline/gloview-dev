#pragma once

#include "../layout.hpp"

namespace gloview {

// What a layout says about a tile, never how it is drawn. The ring is ONE fixed
// size, so a covering box must carry the window's own aspect.
struct TileLook {
  LRect box;
  double round = -1.0; // <0 = look.preview_round
  bool   frame = true; // stroke the ring; off for unfocused run cards
  bool   contain = false; // fit the window whole inside the box
  bool   lift = false; // dragging: taller shadow, no ring, no pill/button
  bool   button = false; // the ✕; the caller owns the GL_BLEND guard
  double tone = 0.0;     // shadow-colour wash over the content, 0..1
};

} // namespace gloview
