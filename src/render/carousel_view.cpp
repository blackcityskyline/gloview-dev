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
//   * depth: zIndex 3-min(d,3), tone wash 0 / 0.125 / 0.25 and thinning past
//     depth 2, selected card emphasized ring
//   * caption on the CENTERED card only, below the preview
//   * counter "n / total" centered above the run
//   * hover: emphasized ring + ✕ in the corner
//
// The CHROME AROUND the preview is the plugin's own, not the reference's: no
// filled card surface (the reference's m_frame/m_previewHost are Material
// surface fills — a solid dark slab around every preview, which fought the
// blurred backdrop), and instead the grid's exact recipe — a soft drop shadow,
// the thin always-on ring plus the thicker focus ring from look.*, the frost
// underlay, and a bare label with no pill. The counter likewise is bare text,
// like every other label the plugin draws.

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
// Same taper the width ladder uses past depth 2: the reference only ever shows
// 5 cards, so it never needed a tone for depth 3+. Holding kFarTone flat would
// stack 6+ cards into an opaque black wall.
constexpr double kDepthTaper = 0.8;

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
  // The metrics layoutTiles resolved from config — NOT fresh defaults: card
  // width (and with it frameInset/aspect scaling of every box) is the user's.
  const auto mt = m_carouselMetrics;

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

    // A card that has left the run (more windows than `carousel_cards`) slides
    // out past the run's edge — currentBox parks it there at its last width —
    // and fades out over the glide. Once the glide is DONE it must stop being
    // drawn entirely: the parked box is a real, full-size rect, so without this
    // the card simply STAYED there for the rest of the session, hanging over
    // the desktop past the right edge of the run.
    float alphaMul = 1.0F;
    if (!card.visible) {
      const double p = tileProgress(i);
      if (p >= 1.0)
        continue;
      alphaMul = static_cast<float>(std::clamp(1.0 - p, 0.0, 1.0));
    }

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
    const auto backingCol = cfg::colors.backing.get();
    // Everything chrome is multiplied by the card's own alpha, so an exiting
    // card fades as one piece (shadow, rings, backing, tone) instead of leaving
    // a hard-edged frame hanging after its content has gone.
    const float a = e * alphaMul;
    const float fa = entryFade(i) * alphaMul;

    // Soft drop shadow, exactly as the grid's drawPreviewChrome does it: the
    // box is PREVIEW-sized (not card-sized) so the shadow hugs the preview the
    // way it does in every other layout, and the alpha is cut hard because the
    // preview itself is usually opaque.
    dropShadow(pv, s, cfg::colors.shadow.get(1.0), 6.0, 16.0,
               static_cast<float>(fa * 0.18), round, roundPow);

    // Frost: the settled backdrop redrawn under a translucent preview, for the
    // same reason as the grid's (see drawPreviewChrome) — sampled from the
    // cached blur at constant alpha over THIS card's own region.
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

    // Rings after the chrome, before the content — the content's own rounded
    // corners then meet the ring's inner edge, same as the grid's tile. The
    // grid's two independent toggles, same precedence.
    if (cfg::look.show_border != 0)
      strokeRing(pv, s, cfg::colors.border.get(a),
                 std::max<int>(1, cfg::look.border_size.get()), round, roundPow);
    if (cfg::look.show_focus_border != 0 && (selected || hovered))
      strokeRing(pv, s, selected ? cfg::colors.select_border.get(a)
                                 : cfg::colors.hover_border.get(a),
                 selected ? std::max<int>(1, cfg::look.select_border_size.get())
                          : std::max<int>(1, cfg::look.hover_border_size.get()),
                 round, roundPow);

    // --- content: CONTAINED in the fixed-aspect preview box ---------------
    // The window keeps its own aspect and is letterboxed inside pv; that is
    // the whole point of the mode (a 16:10 window and a 9:21 window both sit
    // WHOLE inside the same card). Cover would crop instead.
    renderWindowLive(w, m, pxb(pv, s), pxb(pv, s), fa, when, pxr(round, s),
                     roundPow, settled, WindowFit::Contain);

    // --- depth tone: a shadow wash over non-centered previews -------------
    // Fades out with depth so a deep run doesn't turn into a black smear.
    if (card.depth > 0) {
      const double tone =
          (card.depth == 1 ? kNearTone : kFarTone) *
          std::pow(kDepthTaper, static_cast<double>(card.depth - 1));
      g_pHyprOpenGL->renderRect(pxb(pv, s), argb(shadowCol, tone * a),
                                {.round = pxr(round, s),
                                 .roundingPower = roundPow});
    }

    // --- caption: the CENTERED card only, below the preview ---------------
    // The plugin's own title-pill treatment (drawPreviewChrome's), so it reads
    // as the same widget as in every other layout.
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

    // --- ✕ on the hovered card -------------------------------------------
    // closeButtonRect is the SHARED rect (draw here, hit-test in
    // onMouseButton) and honors close_button_position/size, so the drawn and
    // clickable boxes cannot drift apart.
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

  // --- counter: "n / total", centered above the run ----------------------
  // Styled as a plain label — NO pill. Every other label the plugin draws
  // (tile title, workspace name) is bare text over the backdrop; a filled slab
  // here read as a foreign element. It sits centered in the panel the same way
  // the reference's countBackground does, but the box is not painted.
  if (m_tiles.size() > 1) {
    const std::string text =
        std::to_string(m_selected + 1) + " / " + std::to_string(m_tiles.size());
    if (auto tex = cachedLabel(const_cast<char *>("/carousel/count"),
                               text, labelCol, 15);
        tex && tex->m_size.x > 0) {
      const double bw = m_carousel.countW, bh = m_carousel.countH;
      // originX/Y are relative to the area computeCarousel was given (the
      // main box minus the strip band and paddings), not to the monitor.
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

} // namespace gloview
