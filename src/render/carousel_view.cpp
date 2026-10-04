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
  // layoutTiles-resolved metrics, not fresh defaults: card width is the user's.
  const auto mt = m_carouselMetrics;

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

  const auto shadowCol = cfg::colors.shadow.get(1.0);
  const auto labelCol  = cfg::colors.label.get(e);
  const bool  dimmed   = e < 0.999;

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

    const bool selected = card.visible && card.depth == 0;
    const bool hovered  = static_cast<int>(i) == m_hovered;
    const int  round    = clampRound(cardRound, pv.w, pv.h);
    const auto backingCol = cfg::colors.backing.get();
    const float a = e * alphaMul;
    const float fa = entryFade(i) * alphaMul;

    dropShadow(pv, s, cfg::colors.shadow.get(1.0), 6.0, 16.0,
               static_cast<float>(fa * 0.18), round, roundPow);

    if (dimmed && fa > 0.0F) {
      if (const auto btex = backdropBlurTexture(); btex && btex->ok()) {
        const double pxW = static_cast<double>(m->m_pixelSize.x);
        const double pxH = static_cast<double>(m->m_pixelSize.y);
        const CBox  pbPx = pxb(pv, s);
        CTexPassElement::SRenderData td{};
        td.tex           = btex;
        td.box           = pbPx;
        td.a             = 1.0F;
        td.round         = pxr(round, s);
        td.roundingPower = roundPow;
        td.allowCustomUV = true;
        auto &uvTL =
            g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft;
        auto &uvBR =
            g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight;
        uvTL = Vector2D{pbPx.x / pxW, pbPx.y / pxH};
        uvBR = Vector2D{(pbPx.x + pbPx.width) / pxW,
                        (pbPx.y + pbPx.height) / pxH};
        g_pHyprRenderer->draw(td, g_pHyprRenderer->m_renderData.damage);
        uvTL = Vector2D{-1.0, -1.0};
        uvBR = Vector2D{-1.0, -1.0};
        g_pHyprOpenGL->renderRect(
            pbPx, cfg::blur.backdrop.get(a),
            {.round = pxr(round, s), .roundingPower = roundPow});
      }
    }
    safetyBacking(pv, s, backingCol, 0.08 * a, round, roundPow);

    if (cfg::look.show_border != 0)
      strokeRing(pv, s, cfg::colors.border.get(a),
                 std::max<int>(1, cfg::look.border_size.get()), round, roundPow);
    if (cfg::look.show_focus_border != 0 && (selected || hovered))
      strokeRing(pv, s, selected ? cfg::colors.select_border.get(a)
                                 : cfg::colors.hover_border.get(a),
                 selected ? std::max<int>(1, cfg::look.select_border_size.get())
                          : std::max<int>(1, cfg::look.hover_border_size.get()),
                 round, roundPow);

    renderWindowLive(w, m, pxb(pv, s), pxb(pv, s), fa, when, pxr(round, s),
                     roundPow, settled, WindowFit::Contain);

    if (card.depth > 0) {
      const double tone =
          (card.depth == 1 ? kNearTone : kFarTone) *
          std::pow(kDepthTaper, static_cast<double>(card.depth - 1));
      g_pHyprOpenGL->renderRect(pxb(pv, s), argb(shadowCol, tone * a),
                                {.round = pxr(round, s),
                                 .roundingPower = roundPow});
    }

    if (card.showCaption && t.label && t.label->m_size.x > 0 &&
        !swapfxActive(w)) {
      const double lw = t.label->m_size.x, lh = t.label->m_size.y;
      const double padX = 14.0, padY = 6.0;
      const double pw = std::min(pv.w, lw + 2 * padX);
      const double ph = lh + 2 * padY;
      const double px = pv.x + (pv.w - pw) / 2.0;
      const double py = pv.y + pv.h + mt.captionGap - padY;
      g_pHyprOpenGL->renderRect(pxb(LRect{px, py, pw, ph}, s),
                                cfg::colors.title_pill.get(a),
                                {.round = pxr(ph / 2.0, s)});
      g_pHyprOpenGL->renderTexture(
          t.label, pxb(LRect{px + (pw - lw) / 2.0, py + padY, lw, lh}, s),
          {.a = a});
    }

        if (hovered && !closeOnDoubleClick() && m_closeGlyph &&
        m_closeGlyph->m_size.x > 0) {
      const LRect br = closeButtonRect(pv);
      g_pHyprOpenGL->renderRect(pxb(br, s), cfg::colors.close_button.get(a),
                                {.round = pxr(br.h / 2.0, s)});
      const double gw = m_closeGlyph->m_size.x, gh = m_closeGlyph->m_size.y;
      const double gs = std::min((br.w * 0.62) / std::max(1.0, gw),
                                 (br.h * 0.62) / std::max(1.0, gh));
      g_pHyprOpenGL->renderTexture(
          m_closeGlyph,
          pxb(LRect{br.x + (br.w - gw * gs) / 2.0,
                    br.y + (br.h - gh * gs) / 2.0, gw * gs, gh * gs},
              s),
          {.a = a});
    }
  }

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
