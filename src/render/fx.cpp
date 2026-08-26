#include <algorithm>
#include <cctype>
#include <cmath>

#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include "gl_util.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"
#include "../anim/curves.hpp"
#include "window_content.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

// ---- drag visuals (Z3) ------------------------------------------------------

namespace {
// Cursor offset of the drag preview: it sits down-right of the pointer so the
// drop zones it is aimed at stay visible.
constexpr double offX = 46.0, offY = 64.0;
double dragScale() {
  return std::clamp(static_cast<double>(cfg::look.drag_size), 0.15, 1.0);
}
} // namespace

// The picked-up GRID tile's target box: its content box scaled by
// plugin:gloview:drag_size and anchored down-right of the cursor. Desktop/
// canvas mode keeps the true 1:1 grab offset and full size: there a drop
// PARKS the window at exactly that visual spot.
LRect Overview::dragBox() const {
  const int dragIdx = draggedTile();
  if (dragIdx < 0)
    return LRect{0, 0, 0, 0};
  const LRect base = m_tiles[dragIdx].target;
  if (m_desktopMode)
    return LRect{m_drag.x - m_drag.grabDX, m_drag.y - m_drag.grabDY,
                 base.w, base.h};
  const LRect full = tileContentBox(static_cast<size_t>(dragIdx), base);
  const double k   = dragScale();
  return LRect{m_drag.x + offX, m_drag.y + offY, full.w * k, full.h * k};
}

// The picked-up strip card's target box: the full card scaled by drag_size.
LRect Overview::dragStripCardBox() const {
  if (m_drag.idx < 0 || m_drag.idx >= static_cast<int>(m_strip.size()))
    return LRect{0, 0, 0, 0};
  const LRect card = stripCardAt(static_cast<size_t>(m_drag.idx));
  const double k   = dragScale();
  return LRect{m_drag.x + offX, m_drag.y + offY, card.w * k, card.h * k};
}

// The picked-up strip thumb's target box: its slot scaled by drag_size,
// aspect preserved, anchored like the grid preview.
LRect Overview::dragStripBox() const {
  if (m_drag.win.expired())
    return LRect{0, 0, 0, 0};
  const double sw    = m_drag.fromBox.w > 1.0 ? m_drag.fromBox.w : 150.0;
  const double ww    = sw * dragScale();
  const auto size    = m_drag.win.lock()->sizeAnimation()->goal();
  const double aspect = (size.x > 0 && size.y > 0) ? size.x / size.y : 16.0 / 9.0;
  return LRect{m_drag.x + offX, m_drag.y + offY, ww, ww / std::max(0.1, aspect)};
}

// Where the grabbed preview IS right now: flying from its source box to the
// cursor anchor during pickup (the lift leaf shapes time; the destination
// moves with the cursor, so the flight bends naturally), riding at 1 after.
LRect Overview::dragVisualBox() const {
  const LRect to = m_drag.press == model::Drag::Press::Tile     ? dragBox()
                 : m_drag.press == model::Drag::Press::StripCard ? dragStripCardBox()
                                                                  : dragStripBox();
  const double p = curves::eval(m_lift.curve, m_dragLiftClock.raw(m_lift.ms));
  if (p < 1.0 && m_drag.lifted)
    return lerp(m_drag.fromBox, to, p);
  return to;
}

// Z3: chrome for whichever drag is live (grid tile, strip window, or strip card).
void Overview::renderDragTile() const {
  const int dragIdx = draggedTile();
  if (dragIdx >= 0) {
    drawPreviewChrome(static_cast<size_t>(dragIdx), dragVisualBox(),
                      true); // chrome; content right after
    return;
  }
  if (m_drag.press == model::Drag::Press::StripWin && !m_drag.win.expired())
    drawDragStripChrome();
  if (m_drag.press == model::Drag::Press::StripCard)
    drawDragStripCardChrome();
}

// Chrome for a window dragged straight off the strip — same visual language
// as a grid-tile drag (drawPreviewChrome), scaled down to match.
void Overview::drawDragStripChrome() const {
  const auto m = m_monitor.lock();
  const auto w = m_drag.win.lock();
  if (!m || !w)
    return;
  const double s        = m->m_scale;
  const double e        = eased();
  const LRect  lb       = dragVisualBox();
  const int    round    = clampRound(cfg::look.preview_round, lb.w, lb.h);
  const float  roundPow = cfg::look.preview_round_power;
  const auto   shadowCol = cfg::colors.shadow.get(1.0);
  const auto   hoverCol  = cfg::colors.hover_border.get(e);

  g_pHyprOpenGL->renderRoundedShadow(
      pxb(LRect{lb.x, lb.y + 14.0, lb.w, lb.h}, s), pxr(round, s), roundPow,
      static_cast<int>(30.0 * s), Config::CGradientValueData(shadowCol),
      e * 0.18);

  // Real border stroke, not a filled underlay — a filled rect would show
  // through this window's own transparency as a solid wash instead of a
  // frame (see the chrome-kernel comment in gl_util.hpp).
  const int th = cfg::look.hover_border_size; // 0 = no ring
  const Config::CGradientValueData grad(hoverCol);
  g_pHyprOpenGL->renderBorder(pxb(lb, s), grad,
                              {.round = pxr(round, s),
                               .roundingPower = roundPow,
                               .borderSize = th,
                               .a = 1.0F,
                               .outerRound = outerRoundPx(round, th, roundPow, s)});

  safetyBacking(lb, s, cfg::colors.backing.get(), 0.08, round,
                roundPow);
}

// Chrome for a strip CARD dragged off the strip — border + shadow around the
// scaled card, same visual language as tile/strip-window drags.
void Overview::drawDragStripCardChrome() const {
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double s        = m->m_scale;
  const double e        = eased();
  const LRect  lb       = dragVisualBox();
  const int    round    = clampRound(cfg::strip.card_round, lb.w, lb.h);
  const float  roundPow = cfg::look.preview_round_power;
  const auto   shadowCol = cfg::colors.shadow.get(1.0);
  const auto   hoverCol  = cfg::colors.hover_border.get(e);

  g_pHyprOpenGL->renderRoundedShadow(
      pxb(LRect{lb.x, lb.y + 14.0, lb.w, lb.h}, s), pxr(round, s), roundPow,
      static_cast<int>(30.0 * s), Config::CGradientValueData(shadowCol),
      e * 0.18);

  const int th = cfg::look.hover_border_size;
  const Config::CGradientValueData grad(hoverCol);
  g_pHyprOpenGL->renderBorder(pxb(lb, s), grad,
                              {.round = pxr(round, s),
                               .roundingPower = roundPow,
                               .borderSize = th,
                               .a = 1.0F,
                               .outerRound = outerRoundPx(round, th, roundPow, s)});

  safetyBacking(lb, s, cfg::colors.backing.get(), 0.08, round, roundPow);
}

// Z3: the dragged window's live content, right after its chrome. Grid tile
// and strip thumb share one flow: both fly from their source box to the
// cursor anchor on the lift leaf (see dragVisualBox), fully opaque.
// StripCard: render the card's wallpaper thumbnail at the drag position.
void Overview::renderDragWindow() const {
  const auto m = m_monitor.lock();
  if (!m)
    return;

  // StripCard: no window — render the wallpaper thumbnail instead
  if (m_drag.press == model::Drag::Press::StripCard) {
    if (m_drag.idx < 0 || m_drag.idx >= static_cast<int>(m_strip.size()))
      return;
    const auto &it = m_strip[static_cast<size_t>(m_drag.idx)];
    if (it.kind != model::StripItem::Kind::Ws)
      return;
    bool live = false;
    auto tex = backdropSource(live);
    if ((!tex || !tex->ok()) && m_backdropSrcFB && m_backdropSrcFB->isAllocated())
      tex = m_backdropSrcFB->getTexture();
    if (!tex || !tex->ok())
      return;
    const double e     = eased();
    const double scale = m->m_scale;
    const LRect  lb    = dragVisualBox();
    const double texW  = std::max(1.0, static_cast<double>(tex->m_size.x));
    const double texH  = std::max(1.0, static_cast<double>(tex->m_size.y));
    const double cardRatio = lb.w / std::max(1.0, lb.h);
    const double texRatio  = texW / texH;
    double u0 = 0.0, v0 = 0.0, u1 = 1.0, v1 = 1.0;
    if (cardRatio > texRatio) {
      const double f = texRatio / cardRatio;
      v0 = (1.0 - f) / 2.0;
      v1 = (1.0 + f) / 2.0;
    } else {
      const double f = cardRatio / texRatio;
      u0 = (1.0 - f) / 2.0;
      u1 = (1.0 + f) / 2.0;
    }
    CTexPassElement::SRenderData td{};
    td.tex           = tex;
    td.box           = pxb(lb, scale);
    td.a             = static_cast<float>(e);
    td.round         = pxr(clampRound(cfg::strip.card_round, lb.w, lb.h), scale);
    td.roundingPower = cfg::look.preview_round_power;
    td.allowCustomUV = true;
    auto &uvTL = g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft;
    auto &uvBR = g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight;
    uvTL = Vector2D{u0, v0};
    uvBR = Vector2D{u1, v1};
    g_pHyprRenderer->draw(td, g_pHyprRenderer->m_renderData.damage);
    uvTL = Vector2D{-1.0, -1.0};
    uvBR = Vector2D{-1.0, -1.0};
    return;
  }

  PHLWINDOW w;
  if (const int dragIdx = draggedTile(); dragIdx >= 0)
    w = m_tiles[dragIdx].win.lock();
  else if (m_drag.press == model::Drag::Press::StripWin)
    w = m_drag.win.lock();
  if (!w || !w->m_isMapped || w->isHidden())
    return;

  const double e     = eased();
  const double scale = m->m_scale;
  const LRect  lb    = dragVisualBox();
  const CBox   px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
  const int round =
      pxr(clampRound(cfg::look.preview_round, lb.w, lb.h), scale);
  renderWindowLive(w, m, px, px, static_cast<float>(e), Time::steadyNow(),
                   round, cfg::look.preview_round_power);
}

// ---- swap pulses (Z1 tail for grid, Z2 tail for strip) ----------------------

void Overview::kickPulse(const PHLWINDOW &w) {
  if (!w || !leaf("pulse").on)
    return;
  std::erase_if(m_pulses, [&w](const model::WinPulse &p) { return p.w.lock() == w; });
  m_pulses.push_back(model::WinPulse{w, 0.0, std::chrono::steady_clock::now()});
}

// Active swap pulses. strip=true → ring the window's STRIP slot; false → its
// GRID tile box. A window present in both gets pulsed on both surfaces.
// Window-keyed, so rebuilds between kick and render can't orphan it.
void Overview::renderPulses(bool strip) const {
  if (m_pulses.empty())
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double s = m->m_scale;
  const float roundPow = cfg::look.preview_round_power;
  const auto col = cfg::colors.hover_border.get(1.0);
  for (const auto &p : m_pulses) {
    const auto w = p.w.lock();
    if (!w)
      continue;
    const double pr = std::clamp(p.p, 0.0, 1.0);
    if (strip) {
      for (size_t i = 0; i < m_strip.size(); ++i) {
        const auto &it = m_strip[i];
        if (it.kind != model::StripItem::Kind::Ws)
          continue;
        for (size_t j = 0; j < it.wins.size(); ++j) {
          if (it.wins[j].win.lock() != w)
            continue;
          const LRect card = stripCardAt(i);
          const LRect sl = stripWinSlotRect(it, card, j);
          strokeRingPx(pxb(sl, s), col,
                       static_cast<float>((1.0 - pr) * 0.9),
                       clampRound(cfg::look.preview_round,
                                  sl.w, sl.h),
                       roundPow);
        }
      }
    } else {
      for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (m_tiles[i].win.lock() != w)
          continue;
        const LRect lb =
            tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        drawPulseRing(pxb(lb, s),
                      pxr(cfg::look.preview_round, s),
                      roundPow, col, pr);
        break;
      }
    }
  }
}

// Expanding/fading ring flash around a box at pulse progress p (0..1):
// easeOutBack pushes the outline slightly past the box, then it settles back
// while fading out.
void Overview::drawPulseRing(const CBox &boxPx, int round, float roundPow,
                             const CHyprColor &col, double p) const {
  const double k  = curves::eval("back", p) * 0.10;
  const double cx = boxPx.x + boxPx.w / 2.0, cy = boxPx.y + boxPx.h / 2.0;
  const CBox ring{cx - boxPx.w * (0.5 + k), cy - boxPx.h * (0.5 + k),
                  boxPx.w * (1.0 + 2.0 * k), boxPx.h * (1.0 + 2.0 * k)};
  strokeRingPx(ring, col, static_cast<float>((1.0 - p) * 0.9),
               static_cast<int>(std::max(2.0, round + round * 0.5 * k)),
               roundPow);
}

// ---- landings (Z2.5) --------------------------------------------------------

// Flying windows: content drawn at the box lerped from the release point to
// the CURRENT slot box (which may itself be gliding — the lerp targets the
// live slot, so the flight bends toward a moving destination naturally).
// Regular slot rendering for these windows is suppressed while the flight is
// live; at t=1 both boxes coincide and normal rendering takes over invisibly.
void Overview::renderSwapFX() const {
  if (m_swapfx.empty())
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double s       = m->m_scale;
  const auto when      = Time::steadyNow();
  const int  round     = cfg::look.preview_round;
  const float roundPow = cfg::look.preview_round_power;

  for (const auto &fx : m_swapfx) {
    const auto w = fx.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;
    // the CURRENT slot box (it may itself be gliding — the FX targets the
    // live slot and bends toward a moving destination naturally)
    LRect to;
    bool found = false;
    for (size_t i = 0; i < m_tiles.size() && !found; ++i)
      if (m_tiles[i].win.lock() == w) {
        to = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        found = true;
      }
    for (size_t i = 0; i < m_strip.size() && !found; ++i) {
      const auto &it = m_strip[i];
      if (it.kind != model::StripItem::Kind::Ws)
        continue;
      for (size_t j = 0; j < it.wins.size() && !found; ++j)
        if (it.wins[j].win.lock() == w) {
          to = stripWinSlotRect(it, stripCardAt(i), j);
          found = true;
        }
    }
    if (!found)
      continue;

    const double t   = fx.clock.raw(fx.ms);
    const double p   = curves::eval(fx.curve, t);
    double bx = 0, by = 0, bw = 0, bh = 0;
    float alpha = 1.0F;
    bool ring = false; // the release-point ring: only the direct flight keeps it

    if (fx.style == model::SwapStyle::SlideVert) {
      constexpr double LIFT = 12.0; // clearance above the zone's top edge
      if (t < 0.5) { // exit: straight up from the old box
        const double q = curves::eval(fx.curve, t / 0.5);
        bx = fx.from.x;
        by = fx.from.y - (fx.from.h + LIFT) * q;
        bw = fx.from.w;
        bh = fx.from.h;
      } else { // enter: drops down from the top edge into the new box
        const double q = curves::eval(fx.curve, (t - 0.5) / 0.5);
        bx = to.x;
        by = to.y - (to.h + LIFT) * (1.0 - q);
        bw = to.w;
        bh = to.h;
        alpha = static_cast<float>(std::min(1.0, q * 2.0));
      }
    } else if (fx.style == model::SwapStyle::Fade) {
      if (t < 0.5) { // fade out where it was
        bx = fx.from.x; by = fx.from.y; bw = fx.from.w; bh = fx.from.h;
        alpha = static_cast<float>(1.0 - t / 0.5);
      } else { // fade in where it lands
        bx = to.x; by = to.y; bw = to.w; bh = to.h;
        alpha = static_cast<float>((t - 0.5) / 0.5);
      }
    } else if (fx.style == model::SwapStyle::Pop) {
      if (t < 0.3) { // quick fade at the old box
        bx = fx.from.x; by = fx.from.y; bw = fx.from.w; bh = fx.from.h;
        alpha = static_cast<float>(1.0 - t / 0.3);
      } else { // pop into the new box: scale 0.7 -> 1 with a slight overshoot
        const double q = (t - 0.3) / 0.7;
        const double k = 0.7 + 0.3 * curves::eval(leaf("card").curve, q);
        bx = to.x + to.w * (1.0 - k) / 2.0;
        by = to.y + to.h * (1.0 - k) / 2.0;
        bw = to.w * k;
        bh = to.h * k;
        alpha = static_cast<float>(std::min(1.0, q * 3.0));
      }
    } else { // Horizontal: the direct flight, windows travel toward each other
      bx = lerp(fx.from.x, to.x, p);
      by = lerp(fx.from.y, to.y, p);
      bw = lerp(fx.from.w, to.w, p);
      bh = lerp(fx.from.h, to.h, p);
      ring = true;
    }

    const CBox px(bx * s, by * s, bw * s, bh * s);
    const int rpx = pxr(clampRound(round, bw, bh), s);
    // Flight chrome: the drag preview carried a shadow — keep it through the
    // flight (fading with the window) so the release reads as continuous.
    if (alpha > 0.02F)
      g_pHyprOpenGL->renderRoundedShadow(
          pxb(LRect{bx, by + 6.0, bw, bh}, s), rpx, roundPow,
          static_cast<int>(16.0 * s),
          Config::CGradientValueData(argb(cfg::colors.shadow.get(), 1.0)),
          static_cast<float>(0.18 * alpha));
    if (ring && t < 0.6) // the release ring fades quickly on the direct flight
      strokeRingPx(px, argb(cfg::colors.hover_border.get(), (1.0F - t / 0.6F) * 0.9F),
                   (1.0F - t / 0.6F) * 0.9F, rpx, roundPow);
    renderWindowLive(w, m, px, px, alpha, when, rpx, roundPow);
  }
}

// ---- above-layers (Z4) ------------------------------------------------------

bool Overview::isAboveLayer(const std::string &ns) const {
  if (ns.find("aboveoverview") != std::string::npos)
    return true;
  const std::string list = cfg::layer.above_namespaces.get();
  size_t i = 0;
  while (i < list.size()) {
    // split on commas AND whitespace
    while (i < list.size() &&
           (list[i] == ',' || std::isspace(static_cast<unsigned char>(list[i]))))
      ++i;
    size_t start = i;
    while (i < list.size() && list[i] != ',' &&
           !std::isspace(static_cast<unsigned char>(list[i])))
      ++i;
    if (i == start)
      continue;
    std::string entry = list.substr(start, i - start);
    if (entry.back() == '*') {
      const std::string prefix = entry.substr(0, entry.size() - 1);
      if (ns.compare(0, prefix.size(), prefix) == 0)
        return true;
    } else if (ns == entry) {
      return true;
    }
  }
  return false;
}

// Z4: re-render opted-in TOP/OVERLAY layer surfaces (live-input HUDs etc.) on
// top of the overview, via the same immediate leaf as window content —
// IHyprRenderer::renderLayer is renderer-protected, plugins can't call it
// (pinned 0.56.2). damageBox forces a composite even with no client damage.
void Overview::renderAboveLayers() const {
  const auto m = m_monitor.lock();
  if (!m || !g_pHyprRenderer)
    return;
  const auto when = Time::steadyNow();
  for (int idx : {2, 3}) { // LAYER_TOP, LAYER_OVERLAY
    for (const auto &ref : m->m_layerSurfaceLayers[idx]) {
      const auto ls = ref.lock();
      if (!ls || !ls->m_mapped || !ls->wlSurface() ||
          !ls->wlSurface()->resource())
        continue;
      if (!isAboveLayer(ls->m_namespace))
        continue;

      const auto pos =
          ls->positionAnimation()->value(); // absolute logical (layout) coords
      const auto size = ls->sizeAnimation()->value();
      if (!(size.x > 0 && size.y > 0))
        continue;

      g_pHyprRenderer->damageBox(
          CBox{pos.x, pos.y, size.x, size.y});

      CSurfacePassElement::SRenderData data{};
      data.pMonitor = m;
      data.when     = when;
      data.pos      = pos;
      data.w        = std::max(size.x, 1.0);
      data.h        = std::max(size.y, 1.0);
      data.fadeAlpha = 1.F;
      data.alpha    = 1.F;
      data.decorate = false;
      data.rounding = 0;
      data.blur     = false;
      data.surfaceCounter = 0;

      const auto root = ls->wlSurface()->resource();
      root->breadthfirst(
          [&data, &root](SP<CWLSurfaceResource> s, const Vector2D &offset,
                         void *) {
            if (!s || !s->m_current.texture || s->m_current.size.x < 1 ||
                s->m_current.size.y < 1)
              return;
            data.localPos    = offset;
            data.texture     = s->m_current.texture;
            data.surface     = s;
            data.mainSurface = s == root;
            g_pHyprRenderer->draw(data, g_pHyprRenderer->m_renderData.damage);
            data.surfaceCounter++;
          },
          nullptr);
    }
  }
}

} // namespace gloview
