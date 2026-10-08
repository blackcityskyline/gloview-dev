
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

// Read through functions, never cached at namespace scope: see the same note in fx.cpp
// about a global initialised from the config before PLUGIN_INIT.
double nearTone()   { return cfg::carousel.tone_near; }
double farTone()    { return cfg::carousel.tone_far; }
double depthTaper() { return cfg::carousel.depth_taper; }

} // namespace

bool Overview::carouselMode() const {
  return m_view.run();
}

// A selection move is a relayout + glide, detected once: renderStage vs m_view.carouselFor().
void Overview::relayoutCarousel() {
  if (!carouselMode() || m_scene.tiles().empty())
    return;
  // Never mid-drag: a relayout moves every slot, so the run would shift under the
  // window the user is carrying.
  if (draggedTile() >= 0 || m_drag.press == model::Drag::Press::StripWin)
    return;
  // Never during a close: a relayout rewrites every natural, which for maximized
  // windows is one shared rectangle — all the cards land in one point.
  if (!m_opening || m_pendingDeactivate)
    return;
  if (m_scene.selected() < 0 || m_scene.selected() >= static_cast<int>(m_scene.tiles().size()))
    return; // not a selection change; layoutTiles settles it next
  if (m_scene.selected() == m_view.carouselFor())
    return;
  auto shown = captureCurrentBoxes();
  layoutTiles();
  startTileGlide(shown);
}

void Overview::renderCarousel() const {
  const auto m = m_monitor.lock();
  if (!m || m_scene.tiles().empty() || m_view.carousel().cards.size() != m_scene.tiles().size())
    return;

  // Paint order is far -> near by DEPTH, but the cards are still the scene's tiles:
  // slot k is m_view.slot()[k], so the run can be ordered by recency without the
  // array itself being reordered.
  std::vector<int> paint;
  paint.reserve(m_scene.size());
  for (size_t i = 0; i < m_scene.size(); ++i)
    paint.push_back(static_cast<int>(i));
  std::stable_sort(paint.begin(), paint.end(), [this](int a, int b) {
    const auto kOf = [&](const int tile) {
      const int k = m_view.slotOf(static_cast<std::size_t>(tile));
      return static_cast<std::size_t>(k < 0 ? 0 : k);
    };
    const auto &ca = m_view.carousel().cards[kOf(a)];
    const auto &cb = m_view.carousel().cards[kOf(b)];
    if (ca.visible != cb.visible)
      return !ca.visible;
    return ca.depth > cb.depth;
  });

  // Counter first: the cards pass OVER it, so a pushed pill reads as shoved aside instead of
  // sitting on top of the window it is supposed to be yielding to.
  renderCarouselCounter();

  for (const int i : paint) {
    const int k = m_view.slotOf(static_cast<std::size_t>(i));
    const auto &card =
        m_view.carousel().cards[static_cast<std::size_t>(k < 0 ? 0 : k)];
    if (!card.visible)
      continue;
    if (i == draggedTile())
      continue;

    const auto &t = m_scene.tiles()[static_cast<size_t>(i)];
    TileLook look;
    look.box = currentBox(t, i);
    // m_progress runs DOWN on close (1 open -> 0 closed); every exit fraction
    // reads it through exitT. Read backwards it looks exactly like no animation.
    const double exitT = m_opening ? 0.0 : 1.0 - m_progress;
    // The focused card keeps currentBox's own lerp, as in every other layout — one
    // motion, one implementation. An unfocused card is pinned by close() (its real
    // geometry IS the focused card's), so it only shrinks on the close curve.
    if (!m_opening && card.depth != 0) {
      const double k = std::clamp(1.0 - exitT / anim::param(anim::Param::Collapse),
                                  0.0, 1.0);
      const double cx = look.box.cx(), cy = look.box.cy();
      look.box = LRect{cx - look.box.w * k * 0.5, cy - look.box.h * k * 0.5,
                       look.box.w * k, look.box.h * k};
    }
    look.round = cfg::look.preview_round; // one ring size for every card
    look.frame = card.depth == 0;         // only the centred card is framed
    look.contain = true;                  // the window goes inside the box whole
    look.tone =
        card.depth > 0
            ? (card.depth == 1 ? nearTone() : farTone()) *
                  std::pow(depthTaper(), static_cast<double>(card.depth - 1))
            : 0.0;
    look.button = tileCloseButtons();
    drawTile(static_cast<size_t>(i), look);
  }
}

void Overview::renderCarouselCounter() const {
  if (m_scene.tiles().size() < 2)
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double s = m->m_scale;
  const std::string text =
      std::to_string(m_scene.focused() + 1) + " / " + std::to_string(m_scene.tiles().size());
  // Constant colour, like every other label caller. PixelCache keys on (text, maxW) and
  // ignores the colour, so the FIRST render bakes whatever colour that frame had and it is
  // reused until the text changes: passing get(eased()) here meant the counter was baked dim
  // on entry (eased()~0) and only went normal once a switch changed the string (measured,
  // every entry). The fade still rides the draw alpha below -- see .a = eased().
  if (auto tex = m_pixel.fixedLabel(pixel::Slot::CarouselCount, text,
                             cfg::colors.label.get(1.0), 15);
      tex && tex->m_size.x > 0) {
    const double bw = m_view.carousel().countW, bh = m_view.carousel().countH;
    // originX/Y are relative to m_view.carousel().area, not the monitor.
    const double bx =
        m_view.carousel().area.x + m_view.carousel().originX + m_view.carousel().countX;
    const double by =
        m_view.carousel().area.y + m_view.carousel().originY + m_view.carousel().countY;
    // The counter sits on a pill and yields to a preview that reaches it, exactly like the
    // strip band: the overlap depth displaces it, so contact reads as the window pushing it
    // instead of the two drawing over each other. Capped at the pill's own height.
    const LRect pill{bx, by, bw, bh};
    const Vector2D push = pushOut(pill);
    const LRect pillAt{pill.x + push.x, pill.y + push.y, pill.w, pill.h};
    // Radius == half the height with power 1.0 is a true capsule, not a rounded rect.
    g_pHyprOpenGL->renderRect(
        pxb(CBox(pillAt.x, pillAt.y, pillAt.w, pillAt.h), s),
        cfg::colors.carousel_count_pill.get(eased()),
        {.round = pxr(pillAt.h / 2.0, s), .roundingPower = 1.0});
    g_pHyprOpenGL->renderTexture(
        tex,
        pxb(LRect{pillAt.x + (pillAt.w - tex->m_size.x) / 2.0,
                  pillAt.y + (pillAt.h - tex->m_size.y) / 2.0, tex->m_size.x,
                  tex->m_size.y},
            s),
        {.a = static_cast<float>(eased())});
  }
}

} // namespace gloview
