#pragma once

#include "../layout.hpp"

namespace gloview {

// Everything a layout says about a tile. It never says how the tile is drawn:
// Overview::drawTile owns that, in one order, for every layout.
//
// box MUST carry the window's own aspect (Overview::tileBox does this). The surface
// is scaled to cover the box from its centre, so a box of any other aspect
// leaves the content's corner arcs unable to meet the ring's — square content
// inside a round frame, or bare backdrop between the two.
struct TileLook {
  LRect box;
  double round = -1.0; // <0 = look.preview_round
  bool   lift = false; // dragging: taller shadow, no select ring, no pill/button
  bool   button = false; // the ✕; the caller owns the GL_BLEND guard
  double tone = 0.0;     // shadow-colour wash over the content, 0..1
};

} // namespace gloview
