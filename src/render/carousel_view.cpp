// Carousel: cards overlap, so the run is painted in ONE pass sorted far ->
// selected and each card is a whole tile. This file supplies the ORDER and the
// per-card boxes; Overview::drawTile supplies everything a card looks like.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/OpenGL.hpp>

#include "gl_util.hpp"
#include "tile_look.hpp"
#include "window_content.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

// Depth darkening: the run recedes instead of reading as N equal previews.
constexpr double kNearTone = 0.125;
constexpr double kFarTone = 0.25;
constexpr double kDepthTaper = 0.8;

} // namespace

// What layoutTiles actually built, not what the config asks for: free-arrange,
// the all/one flip and the carousel switch each override it, and a renderer that
// read the config would reach for an arrangement that was never produced.
bool Overview::carouselMode() const {
  return m_run;
}

// A selection move is a relayout + glide, detected once: renderStage vs m_carouselFor.
void Overview::relayoutCarousel() {
  if (!carouselMode() || m_tiles.empty())
    return;
  if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
    return; // not a selection change; layoutTiles settles it next
  if (m_selected == m_carouselFor)
    return;
  auto shown = captureCurrentBoxes();
  layoutTiles();
  startTileGlide(shown);
}

void Overview::renderCarousel() const {
  const auto m = m_monitor.lock();
  if (!m || m_tiles.empty() || m_carousel.cards.size() != m_tiles.size())
    return;

  std::vector<int> order;
  order.reserve(m_tiles.size());
  for (size_t i = 0; i < m_tiles.size(); ++i)
    order.push_back(static_cast<int>(i));
  std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
    const auto &ca = m_carousel.cards[static_cast<size_t>(a)];
    const auto &cb = m_carousel.cards[static_cast<size_t>(b)];
    if (ca.visible != cb.visible)
      return !ca.visible;
    return ca.depth > cb.depth;
  });

  for (const int i : order) {
    const auto &card = m_carousel.cards[static_cast<size_t>(i)];
    // A card off the run is not part of the carousel: it has no box, and the
    // one it is shown at is its slot from before it left. Painting it there is
    // what made a window sail across the backdrop on every selection change.
    if (!card.visible)
      continue;

    TileLook look;
    look.box = currentBox(m_tiles[static_cast<size_t>(i)], i);
    look.round = cfg::look.preview_round; // one ring size for every card
    look.frame = card.depth == 0;         // only the centred card is framed
    look.contain = true;                  // the window goes inside the box whole
    look.tone =
        card.depth > 0
            ? (card.depth == 1 ? kNearTone : kFarTone) *
                  std::pow(kDepthTaper, static_cast<double>(card.depth - 1))
            : 0.0;
    look.button = tileCloseButtons();
    drawTile(static_cast<size_t>(i), look);
  }

  renderCarouselCounter();
}

void Overview::renderCarouselCounter() const {
  if (m_tiles.size() < 2)
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double s = m->m_scale;
  const std::string text =
      std::to_string(m_selected + 1) + " / " + std::to_string(m_tiles.size());
  if (auto tex = cachedLabel(const_cast<char *>("/carousel/count"), text,
                             cfg::colors.label.get(eased()), 15);
      tex && tex->m_size.x > 0) {
    const double bw = m_carousel.countW, bh = m_carousel.countH;
    // originX/Y are relative to m_carousel.area, not the monitor.
    const double bx =
        m_carousel.area.x + m_carousel.originX + m_carousel.countX;
    const double by =
        m_carousel.area.y + m_carousel.originY + m_carousel.countY;
    g_pHyprOpenGL->renderTexture(
        tex,
        pxb(LRect{bx + (bw - tex->m_size.x) / 2.0,
                  by + (bh - tex->m_size.y) / 2.0, tex->m_size.x,
                  tex->m_size.y},
            s),
        {.a = static_cast<float>(eased())});
  }
}

} // namespace gloview
