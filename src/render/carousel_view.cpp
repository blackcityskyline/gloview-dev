// Carousel layout mode (plugin:gloview:layout == "carousel").
//
// Geometry is NOT here — layout.cpp owns it (computeCarousel), ported 1:1 from
// noctalia v5.2's window switcher. This file is the paint half: the reference
// draws every card as ONE subtree (frame → preview host → contained thumbnail
// → depth tone → caption), and cards OVERLAP, so a card's own window content
// has to land between its frame and the next card's frame. The grid's split Z1
// stages (all chrome, then all content) cannot express that — every far card's
// window would paint over the selected card's border. Hence one self-contained
// pass, depth-sorted far → selected, and paint() skips the grid stages entirely
// in this mode.
//
// What is ported 1:1 from window_switcher_carousel_style.cpp /
// window_switcher_tile.cpp:
//   * card slot geometry (computeCarousel, layout.cpp)
//   * FIXED-aspect preview box, window CONTAINED inside it (letterboxed on its
//     own aspect) — renderWindowLive(..., WindowFit::Contain)
//   * depth: zIndex 3-min(d,3), tone overlay alpha 0 / 0.125 / 0.25, selected
//     card emphasized ring, others a thin one
//   * caption on the SELECTED card only, full card width, below the frame
//   * counter "n / total" centered above the run
//   * hover: emphasized ring + ✕ in the corner; close ✕ on the hovered card
// Deliberately NOT ported (we have our own equivalents, and the reference's
// are shell-UI chrome, not layout): app icon + fallback glyph in the preview,
// its drop shadow, and its open/close scale-slide (our own tile glide covers
// the motion).

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

// The reference's depth tone, as fractions of ColorRole::Shadow
// (kNearToneAlpha = disabledOutlineAlpha*0.25, kFarToneAlpha = *0.5 —
// window_switcher_tile.cpp). We multiply our own shadow color by the same
// factors: a far card reads as clearly recessed, a near one barely touched.
constexpr double kNearTone = 0.125;
constexpr double kFarTone = 0.25;

} // namespace

bool Overview::carouselMode() const {
  return parseEngine(cfg::grid.layout.get().c_str()) == Engine::Carousel;
}

// The carousel's slots are built AROUND m_selected, so moving the selection
// is a relayout rather than a repaint. Detected in ONE place — renderStage,
// comparing m_carouselFor — so every existing way of moving the selection
// (arrow keys, alt-tab, a click, focus_follows_mouse) rotates the run without
// any of them knowing this mode exists. Same machinery a drop/reflow uses, so
// the motion and the anim pump are the ones already proven for a mid-session
// rebuild.
void Overview::relayoutCarousel() {
  if (!carouselMode() || m_tiles.empty())
    return;
  if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
    return; // not a selection change; the next layoutTiles settles it
  if (m_selected == m_carouselFor)
    return;
  auto shown = captureCurrentBoxes();
  layoutTiles();
  startTileGlide(shown);
}

// Cards must paint far → selected (the reference's zIndex 3-min(d,3) does the
// same job for its scene graph): a nearer card overlaps the ones beside it, so
// drawing the selected one last is what puts its emphasized ring on top.
// Off-run cards (visible == false) are painted LAST, after everything in the
// run, and only while their exit glide is live — currentBox() slides them past
// the run's edge and renderCarousel stops drawing them once their box collapses
// to zero, which is the reference's outgoing-card slide-then-hide.
void Overview::renderCarousel() const {
  const auto m = m_monitor.lock();
  if (!m || m_tiles.empty() || m_carousel.cards.size() != m_tiles.size())
    return;

  const double s        = m->m_scale;
  const double e        = eased();
  const auto   when     = Time::steadyNow();
  const int    cardRound = cfg::look.preview_round;
  const float  roundPow = cfg::look.preview_round_power;
  // Carousel slots only change on a selection move / rebuild, never per frame,
  // so a settled box is frame-stable: pad=0 in the content blit (the exact
  // same argument as the grid's tileContentBox call site).
  const bool settled = m_tileClock.done(glideDur());
  const auto mt      = CarouselMetrics{};

  // depth 3 → 0. Stable within a depth (sort is not stable, and equal depths
  // must not swap paint order frame to frame).
  std::vector<int> order;
  order.reserve(m_tiles.size());
  for (size_t i = 0; i < m_tiles.size(); ++i)
    order.push_back(static_cast<int>(i));
  std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
    const auto &ca = m_carousel.cards[static_cast<size_t>(a)];
    const auto &cb = m_carousel.cards[static_cast<size_t>(b)];
    // Off-run cards last: they have no depth of their own (depth stays at its
    // default), and they are on their way out — nothing should paint over a
    // card that is still in the run.
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

    // The ANIMATED card rect (mid-glide between the old and new slot), not the
    // target — that is the whole carousel transition.
    const LRect box = currentBox(t, i);
    if (box.w < 1.0 || box.h < 1.0)
      continue;
    const LRect pv = carouselPreviewBox(box, mt);
    if (pv.w < 1.0 || pv.h < 1.0)
      continue;

    const bool selected = card.visible && card.depth == 0;
    const bool hovered  = static_cast<int>(i) == m_hovered;
    const int  round    = clampRound(cardRound, pv.w, pv.h);
    const auto frameCol = cfg::colors.backing.get();
    const auto ringCol  = selected ? cfg::colors.select_border.get(e)
                                   : (hovered ? cfg::colors.hover_border.get(e)
                                              : cfg::colors.border.get(e));
    const int ringSize =
        (selected || hovered)
            ? std::max<int>(1, cfg::look.hover_border_size.get())
            : std::max<int>(1, cfg::look.border_size.get());

    // --- frame: the card surface the preview sits inset in ---------------
    // (the reference's m_frame + m_previewHost: previewHost is inset by
    // spaceXs INSIDE the frame, so the frame shows as a thin surface margin
    // around the preview — pv is exactly that inset box, see
    // carouselPreviewBox).
    g_pHyprOpenGL->renderRect(
        pxb(box, s), frameCol,
        {.round = pxr(cardRound, s), .roundingPower = roundPow});

    // Frost: the settled backdrop redrawn under a translucent preview, for the
    // same reason as the grid's (see drawPreviewChrome) — sampled from the
    // cached blur at constant alpha over THIS card's own region.
    if (dimmed) {
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
            pbPx, cfg::blur.backdrop.get(static_cast<float>(e)),
            {.round = pxr(round, s), .roundingPower = roundPow});
      }
    }
    safetyBacking(pv, s, frameCol, 0.08, round, roundPow);

    // Ring last of the chrome, so the window content below can't cover it.
    if (cfg::look.show_border != 0 || selected || hovered)
      strokeRing(pv, s, ringCol, ringSize, round, roundPow);

    // --- content: CONTAINED in the fixed-aspect preview box ---------------
    // The window keeps its own aspect and is letterboxed inside pv; that is
    // the whole point of the mode (a 16:10 window and a 9:21 window both sit
    // WHOLE inside the same card). Cover would crop instead.
    renderWindowLive(w, m, pxb(pv, s), pxb(pv, s),
                     static_cast<float>(entryFade(i)), when, pxr(round, s),
                     roundPow, settled, WindowFit::Contain);

    // --- depth tone: a shadow wash over non-selected previews -------------
    if (card.depth > 0) {
      const double tone = card.depth == 1 ? kNearTone : kFarTone;
      g_pHyprOpenGL->renderRect(pxb(pv, s), argb(shadowCol, tone * e),
                                {.round = pxr(round, s),
                                 .roundingPower = roundPow});
    }

    // --- caption: the SELECTED card only, full card width, below the frame --
    if (card.showCaption && t.label && t.label->m_size.x > 0 &&
        !swapfxActive(w)) {
      const double lw = t.label->m_size.x, lh = t.label->m_size.y;
      const double padX = 14.0, padY = 6.0;
      const double cw = std::min(box.w, lw + 2 * padX);
      const double ch = lh + 2 * padY;
      const double cx = box.x + (box.w - cw) / 2.0;
      const double cy = box.y + pv.h + mt.frameInset + mt.captionGap;
      g_pHyprOpenGL->renderRect(pxb(LRect{cx, cy, cw, ch}, s),
                                cfg::colors.title_pill.get(e),
                                {.round = pxr(ch / 2.0, s)});
      g_pHyprOpenGL->renderTexture(
          t.label,
          pxb(LRect{cx + (cw - lw) / 2.0, cy + padY, lw, lh}, s),
          {.a = static_cast<float>(e)});
    }

    // --- ✕ on the hovered card -------------------------------------------
    // closeButtonRect is the SHARED rect (draw here, hit-test in
    // onMouseButton) and honors close_button_position/size, so the drawn and
    // clickable boxes cannot drift apart.
    if (hovered && !closeOnDoubleClick() && m_closeGlyph &&
        m_closeGlyph->m_size.x > 0) {
      const LRect br = closeButtonRect(box);
      g_pHyprOpenGL->renderRect(pxb(br, s), cfg::colors.close_button.get(e),
                                {.round = pxr(br.h / 2.0, s)});
      const double gw = m_closeGlyph->m_size.x, gh = m_closeGlyph->m_size.y;
      const double gs = std::min((br.w * 0.62) / std::max(1.0, gw),
                                 (br.h * 0.62) / std::max(1.0, gh));
      g_pHyprOpenGL->renderTexture(
          m_closeGlyph,
          pxb(LRect{br.x + (br.w - gw * gs) / 2.0,
                    br.y + (br.h - gh * gs) / 2.0, gw * gs, gh * gs},
              s),
          {.a = static_cast<float>(e)});
    }
  }

  // --- counter: "n / total", centered above the run ----------------------
  // (the reference's countBackground + counterLabel, sized controlHeightLg*2 x
  // controlHeightSm and centered on the panel).
  if (m_tiles.size() > 1) {
    const std::string text =
        std::to_string(m_selected + 1) + " / " + std::to_string(m_tiles.size());
    if (auto tex = cachedLabel(const_cast<char *>("/carousel/count"),
                               text, labelCol, 13);
        tex && tex->m_size.x > 0) {
      const double bw = m_carousel.countW, bh = m_carousel.countH;
      // originX/Y are relative to the area computeCarousel was given (the
      // main box minus the strip band and paddings), not to the monitor.
      const double bx = m_carousel.area.x + m_carousel.originX + m_carousel.countX;
      const double by = m_carousel.area.y + m_carousel.originY + m_carousel.countY;
      g_pHyprOpenGL->renderRect(
          pxb(LRect{bx, by, bw, bh}, s), cfg::colors.backing.get(),
          {.round = pxr(bh / 3.0, s), .roundingPower = roundPow});
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

} // namespace gloview
