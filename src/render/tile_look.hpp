#pragma once

#include "../layout.hpp"

namespace gloview {

// Everything a layout says about a tile. It never says how the tile is drawn:
// Overview::drawTile owns that, in one order, for every layout.
//
// The ring is ONE fixed size everywhere and is stroked on the tile's own box,
// exactly as in every other layout: it is not clamped down for a small tile,
// because a tile that changes its own frame radius stops being the same shape at
// a smaller size.
//
// The window COVERS its box by default, so the box must carry the window's own
// aspect (Overview::tileBox does this) — any other aspect pushes the content
// past the ring far enough that the corner arcs cannot meet.
//
struct TileLook {
  LRect box;
  double round = -1.0; // <0 = look.preview_round
  bool   frame = true; // stroke the ring; off for the run's unfocused cards
  bool   contain = false; // fit the window whole inside the box
  bool   lift = false; // dragging: taller shadow, no select ring, no pill/button
  bool   button = false; // the ✕; the caller owns the GL_BLEND guard
  double tone = 0.0;     // shadow-colour wash over the content, 0..1
};

} // namespace gloview
