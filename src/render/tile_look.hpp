#pragma once

#include "../layout.hpp"

namespace gloview {

// Everything a layout says about a tile. It never says how the tile is drawn:
// Overview::drawTile owns that, in one order, for every layout.
//
// A tile normally shows its window COVERING the box, so the box must carry the
// window's own aspect (Overview::tileBox does this) — any other aspect leaves the
// content sticking out past the ring far enough that the corner arcs cannot
// meet, and the frame reads as round around square content.
//
// `card` is the other shape, for a run that needs ONE card size: the box is a
// fixed-aspect panel, the window is contained inside it whole, and the panel is
// filled so the leftover margin is the card's own surface rather than bare
// backdrop. Contained content never over-covers, so the ring still matches.
struct TileLook {
  LRect box;
  double round = -1.0; // <0 = look.preview_round
  bool   lift = false; // dragging: taller shadow, no select ring, no pill/button
  bool   button = false; // the ✕; the caller owns the GL_BLEND guard
  bool   card = false;   // fixed-aspect panel, window contained, margin filled
  double tone = 0.0;     // shadow-colour wash over the content, 0..1
};

} // namespace gloview
