// Carousel mode: cards overlap, so ONE paint pass sorted far -> selected; the
// FIXED-aspect preview CONTAINS the window (letterboxed), never Cover.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "gl_util.hpp"
#include "window_content.hpp"
#include "../anim/curves.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

// Tone = fraction of the shadow color; taper past depth 2 so 6+ cards do not black out.
constexpr double kNearTone = 0.125;
constexpr double kFarTone = 0.25;
constexpr double kDepthTaper = 0.8;

}

bool Overview::carouselMode() const {
  return parseEngine(cfg::grid.layout.get().c_str()) == Engine::Carousel;
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

  const double s        = m->m_scale;
  const double e        = eased();
  const auto   when     = Time::steadyNow();
  const int    cardRound = cfg::look.preview_round;
  const float  roundPow = cfg::look.preview_round_power;
  // Slots move only on a selection change, so a settled box is frame-stable: pad=0.
  const bool settled = m_tileClock.done(glideDur());
  const auto mt = m_carouselMetrics; // layoutTiles-resolved: width is the user's

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

  const auto labelCol = cfg::colors.label.get(e);
  // The ✕ goes after EVERY card: inside the loop a nearer one covers it.
  LRect hoverBox{};
  bool  hoverBtn = false;

  for (const int i : order) {
    const auto &card = m_carousel.cards[static_cast<size_t>(i)];
    const auto &t    = m_tiles[static_cast<size_t>(i)];
    const auto  w    = t.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;

    float alphaMul = 1.0F;
    if (!card.visible) {
      const double p = tileProgress(i);
      if (p >= 1.0)
        continue;
      alphaMul = static_cast<float>(std::clamp(1.0 - p, 0.0, 1.0));
    }

    const LRect box = currentBox(t, i);
    if (box.w < 1.0 || box.h < 1.0)
      continue;
    const LRect pv = carouselPreviewBox(box, mt);
    if (pv.w < 1.0 || pv.h < 1.0)
      continue;

    const int  round = clampRound(cardRound, pv.w, pv.h);
    const float fa   = entryFade(i) * alphaMul;

    drawPreviewChrome(static_cast<size_t>(i), pv, false, alphaMul);

    renderWindowLive(w, m, pxb(pv, s), pxb(pv, s), fa, when, pxr(round, s),
                     roundPow, settled, WindowFit::Contain);

    // Depth darkening: the run reads as receding cards, not as N equal previews.
    if (card.depth > 0) {
      const double tone =
          (card.depth == 1 ? kNearTone : kFarTone) *
          std::pow(kDepthTaper, static_cast<double>(card.depth - 1));
      g_pHyprOpenGL->renderRect(
          pxb(pv, s), argb(cfg::colors.shadow.get(1.0), tone * fa),
          {.round = pxr(round, s), .roundingPower = roundPow});
    }

    if (i == m_hovered && !closeOnDoubleClick() && card.visible) {
      hoverBox  = pv;
      hoverBtn  = true;
    }
  }

  if (hoverBtn)
    drawCloseButton(hoverBox, static_cast<float>(e));

  if (m_tiles.size() > 1) {
    const std::string text =
        std::to_string(m_selected + 1) + " / " + std::to_string(m_tiles.size());
    if (auto tex = cachedLabel(const_cast<char *>("/carousel/count"),
                               text, labelCol, 15);
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
          {.a = static_cast<float>(e)});
    }
  }
}

}
