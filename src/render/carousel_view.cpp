// Cards overlap, so the run is painted in ONE pass, far -> selected, each card a
// whole tile. This file supplies the order and the boxes; drawTile the looks.

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
#include "../debug/log.hpp"
#include "../overview.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

constexpr double kNearTone = 0.125;
constexpr double kFarTone = 0.25;
constexpr double kDepthTaper = 0.8;

} // namespace

// What layoutTiles actually built, not what the config asks for.
bool Overview::carouselMode() const {
  return m_run;
}

// A selection move is a relayout + glide, detected once: renderStage vs m_carouselFor.
void Overview::relayoutCarousel() {
  if (!carouselMode() || m_tiles.empty())
    return;
  // Never mid-drag: a relayout moves every slot, so the run would shift under the
  // window the user is carrying.
  if (draggedTile() >= 0 || m_drag.press == model::Drag::Press::StripWin)
    return;
  // Never during a close: a relayout rewrites every natural, which for a run of
  // maximized windows is one shared rectangle — all the cards land in one point.
  if (!m_opening || m_pendingDeactivate)
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
    // A card off the run has no box; the one it is shown at is its slot from
    // before it left, and painting it there sails it across the backdrop.
    if (!card.visible)
      continue;
    // The dragged card is already on screen under the cursor; painting its slot
    // too showed the preview twice.
    if (i == draggedTile())
      continue;

    const auto &t = m_tiles[static_cast<size_t>(i)];
    TileLook look;
    look.box = currentBox(t, i);
    // m_progress runs DOWN on close (1 open -> 0 closed); every exit fraction
    // reads it through exitT. Read backwards it looks exactly like no animation.
    const double exitT = m_opening ? 0.0 : 1.0 - m_progress;
    // The focused card keeps currentBox's own lerp, as in every other layout — one
    // motion, one implementation. An unfocused card is pinned by close() (its real
    // geometry IS the focused card's, so flying home drops it under the landing),
    // so it only shrinks away on the close curve.
    if (!m_opening && card.depth != 0) {
      const double k = std::clamp(1.0 - exitT / 0.5, 0.0, 1.0);
      const double cx = look.box.cx(), cy = look.box.cy();
      look.box = LRect{cx - look.box.w * k * 0.5, cy - look.box.h * k * 0.5,
                       look.box.w * k, look.box.h * k};
    }
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
  if (auto tex = m_pixel.label(const_cast<char *>("/carousel/count"), text,
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
