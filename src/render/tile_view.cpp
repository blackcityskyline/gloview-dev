#include <algorithm>
#include <cmath>
#include <ctime>

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include "gl_util.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"
#include "../debug/log.hpp"
#include "../anim/curves.hpp"
#include "window_content.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

LRect fitInside(const LRect &outer, double aspect) {
  if (outer.w <= 0.0 || outer.h <= 0.0 || aspect <= 0.0)
    return outer;

  const double outerAspect = outer.w / outer.h;
  if (std::abs(outerAspect - aspect) <= 0.01)
    return outer;

  if (outerAspect > aspect) {
    const double w = outer.h * aspect;
    return LRect{outer.x + (outer.w - w) / 2.0, outer.y, w, outer.h};
  }

  const double h = outer.w / aspect;
  return LRect{outer.x, outer.y + (outer.h - h) / 2.0, outer.w, h};
}

} // namespace

// plugin:gloview:close_button_visibility == "always" — show close buttons on
// every tile and strip card all the time, not just in desktop mode / while
// Shift is the desktop-mode key.
bool Overview::closeButtonsAlwaysOn() const {
  return cfg::look.close_button_visibility.get() == "always";
}

// plugin:gloview:close_trigger == "doubleclick" (default "button"): swaps the
// per-window "✕" entirely for a double-click directly on the tile — see the
// deferred single-click handling in onMouseButton. Only affects the
// PER-WINDOW close mechanism.
bool Overview::closeOnDoubleClick() const {
  return cfg::look.close_trigger.get() == "doubleclick";
}

LRect Overview::closeButtonRect(const LRect &lb) const {
  const double scale = std::max(0.3, static_cast<double>(
                                         cfg::look.close_button_size));
  const double r     = std::clamp(std::min(lb.w, lb.h) * 0.11, 9.0, 18.0) * scale;
  const double inset = r + 6.0;
  const std::string pos = cfg::look.close_button_position.get();
  double cx = lb.x + lb.w - inset, cy = lb.y + inset; // top-right (default)
  if (pos == "top-left") {
    cx = lb.x + inset;
    cy = lb.y + inset;
  } else if (pos == "bottom-right") {
    cx = lb.x + lb.w - inset;
    cy = lb.y + lb.h - inset;
  } else if (pos == "bottom-left") {
    cx = lb.x + inset;
    cy = lb.y + lb.h - inset;
  }
  return LRect{cx - r, cy - r, 2 * r, 2 * r};
}

// Fit `slot` to the window's real aspect so the live surface fills it exactly
// (uniform scale) — tile chrome, backing and content share one rect.
// Desktop (canvas) mode: the slot already carries the window's parked aspect;
// use it AS-IS so a survivor Hyprland re-tiled to a new shape keeps its
// frozen preview shape.
LRect Overview::tileContentBox(size_t i, const LRect &slot) const {
  if (m_desktopMode)
    return slot;
  double aspect = slot.w / std::max(1.0, slot.h);
  if (i < m_tiles.size()) {
    if (const auto w = m_tiles[i].win.lock()) {
      const auto s = w->sizeAnimation()->goal();
      if (s.x > 0 && s.y > 0)
        aspect = s.x / s.y;
    }
  }
  return fitInside(slot, aspect);
}

// Content/chrome alpha multiplier for a populating tile: 1 everywhere except
// the "fade" entry style (ws switches and the all<->one flip, both carry
// m_wsSlideDir), where the preview ramps in with its appear progress — chrome
// and content fade together so no full-strength ring ever sits under a
// transparent body.
double Overview::entryFade(size_t i) const {
  if (i >= m_tiles.size())
    return 1.0;
  const auto &t = m_tiles[i];
  if (t.appear >= 1.0 || m_wsSlideDir == 0 || m_enterStyle != "fade")
    return 1.0;
  return tileAppear(static_cast<int>(i));
}

// One grid tile's chrome: shadow → rings → frost → backing → title pill.
// Runs in the painter's Z1, BEFORE the tile's content — everything here sits
// under the live surface by construction, so no ordering tricks are needed.
void Overview::drawPreviewTile(size_t i, const LRect &slot, bool lift) const {
  const auto m = m_monitor.lock();
  if (!m || i >= m_tiles.size())
    return;
  const double s        = m->m_scale;
  const double e        = eased();
  const float  f        = static_cast<float>(entryFade(i));
  const int    round    = cfg::look.preview_round;
  const float  roundPow = cfg::look.preview_round_power;
  const auto shadowCol  = cfg::colors.shadow.get(1.0);
  const auto hoverCol   = cfg::colors.hover_border.get(e * f);

  const auto &t = m_tiles[i];
  const auto w  = t.win.lock();
  if (!w || !w->m_isMapped || w->isHidden())
    return;

  const LRect lb = tileContentBox(i, slot);

  // Soft drop shadow (real gaussian). The box is TILE-SIZED, just shifted
  // down: its solid core sits inside the tile footprint and shows through
  // genuinely transparent windows, so the alpha is cut hard (×0.18) — depth
  // cue, not a second layer. roundingPower matches the content's.
  dropShadow(lb, s, shadowCol, lift ? 14.0 : 6.0, lift ? 30.0 : 16.0,
             e * 0.18F * f, round, roundPow);

  const bool framed   = (static_cast<int>(i) == m_hovered || lift);
  const bool selected = (static_cast<int>(i) == m_selected) && !lift;

  // Two independent, modular ring layers (see README "Border modes"):
  // show_border — an always-on base ring on EVERY tile; show_focus_border —
  // the hover/keyboard-selection ring on top (hover wins over selection).
  if (cfg::look.show_border != 0) {
    strokeRing(lb, s, cfg::colors.border.get(e * f),
               cfg::look.border_size, round, roundPow);
  }
  if (cfg::look.show_focus_border != 0) {
    if (framed)
      strokeRing(lb, s, hoverCol,
                 cfg::look.hover_border_size, round,
                 roundPow);
    else if (selected)
      strokeRing(lb, s, cfg::colors.select_border.get(e * f),
                 cfg::look.select_border_size, round,
                 roundPow);
  }

  // Transition frost: while the global backdrop is still fading (entry) or
  // the tile is popping in (population), a translucent tile's see-through
  // pixels blend against the SHARP desktop under the semi-transparent
  // backdrop — a visible dip toward sharp and a switch from the real
  // desktop's blur-behind. The frost is a LOCAL REDRAW OF THE SETTLED
  // BACKDROP, constrained to the tile: the cached blur at CONSTANT alpha
  // (any fade re-opens a window for the sharp component: compositing
  // backdrop-blur e and frost (1-e) over currentFB leaves C*e*(1-e) sharp
  // leakage — the "blur dips to half strength mid-flight" artifact) plus the
  // dim rect tracking the GLOBAL dim alpha e, so the interior's dim equals
  // the surroundings' dim in every frame (a constant-1.0 dim made a darker
  // region travel with the moving tile and jump by a full dim step at the
  // e >= 0.999 boundary). Both draws are ROUNDED like the content (a rect
  // scissor leaves ghost sharp corners) and the blit is UV-mapped to the
  // tile's own region of the monitor-sized blur texture (a plain
  // renderTexture would stretch the whole texture into the box). At
  // e >= 0.999 the block stops — by then frost and backdrop are
  // pixel-identical, so the boundary is invisible.
  const double apNow =
      t.appear < 1.0 ? tileAppear(static_cast<int>(i)) : 1.0;
  if (e < 0.999 || apNow < 0.999) {
    if (const auto btex = backdropBlurTexture(); btex && btex->ok()) {
      const double pxW = static_cast<double>(m->m_pixelSize.x);
      const double pxH = static_cast<double>(m->m_pixelSize.y);
      const CBox  lbPx = pxb(lb, s);
      CTexPassElement::SRenderData td{};
      td.tex           = btex;
      td.box           = lbPx;
      td.a             = 1.0F;
      td.round         = pxr(round, s);
      td.roundingPower = roundPow;
      td.allowCustomUV = true;
      auto &uvTL = g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft;
      auto &uvBR = g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight;
      uvTL = Vector2D{lbPx.x / pxW, lbPx.y / pxH};
      uvBR = Vector2D{(lbPx.x + lbPx.width) / pxW,
                      (lbPx.y + lbPx.height) / pxH};
      g_pHyprRenderer->draw(td, g_pHyprRenderer->m_renderData.damage);
      uvTL = Vector2D{-1.0, -1.0};
      uvBR = Vector2D{-1.0, -1.0};
      // the frost covers the global dim drawn earlier — re-apply it at the
      // global alpha so the interior's dim tracks the surroundings exactly
      g_pHyprOpenGL->renderRect(
          lbPx,
          cfg::blur.backdrop.get(static_cast<float>(e)),
          {.round = pxr(round, s), .roundingPower = roundPow});
    }
  }
  safetyBacking(lb, s, cfg::colors.backing.get(), 0.08, round,
                roundPow);

  // window title in a dark pill below the tile (hover or keyboard selection)
  if ((framed || selected) && !lift && t.label && t.label->m_size.x > 0) {
    const double lw   = t.label->m_size.x;
    const double lh   = t.label->m_size.y;
    const double padX = 14.0, padY = 6.0;
    const double pw   = lw + 2 * padX;
    const double ph   = lh + 2 * padY;
    const double px   = std::clamp(lb.cx() - pw / 2.0, 6.0, m->m_size.x - pw - 6.0);
    const double py   = std::min(lb.y + lb.h + 10.0, m->m_size.y - ph - 6.0);
    g_pHyprOpenGL->renderRect(pxb(CBox(px, py, pw, ph), s),
                              cfg::colors.title_pill.get(e * f),
                              {.round = pxr(ph / 2.0, s)});
    g_pHyprOpenGL->renderTexture(t.label,
                                 pxb(CBox(px + padX, py + padY, lw, lh), s),
                                 {.a = static_cast<float>(e * f)});
  }
}

// Z1: every non-dragged tile's chrome, painter order (top-left to
// bottom-right).
void Overview::renderPreviews() const {
  const int dragIdx = draggedTile();
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    if (static_cast<int>(i) == dragIdx)
      continue; // the dragged tile floats over the strip; drawn later
    drawPreviewTile(i, currentBox(m_tiles[i], static_cast<int>(i)), false);
  }
}

// Per-window "✕" (desktop mode or close_button_visibility=always). Drawn
// AFTER the tile content in the painter's Z1: the button sits INSIDE the tile
// bounds, so drawing it before the surface let opaque window content paint
// straight over it.
void Overview::renderTileButtons() const {
  if (closeOnDoubleClick())
    return; // doubleclick mode replaces the per-window "✕" entirely
  if (!m_desktopMode && !closeButtonsAlwaysOn())
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  // Defensive reset before drawing the ✕ glyph, fully restored below: with
  // GL_BLEND left disabled by a previous pass, the glyph texture's
  // transparent padding writes through as opaque black. Blend state (both
  // the enable flag and the exact func) is saved and put back — this runs
  // right before the strip's band/cards, and leaving OUR straight-alpha func
  // in place would hand the same leak to them.
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  const bool blendBefore = glIsEnabled(GL_BLEND) == GL_TRUE;
  GLint blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcAlpha = GL_ONE,
        blendDstAlpha = GL_ZERO;
  glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
  glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  const double s       = m->m_scale;
  const double e       = eased();
  const int    dragIdx = draggedTile();
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    if (static_cast<int>(i) == dragIdx)
      continue;
    const auto w = m_tiles[i].win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;
    const LRect lb = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
    const LRect br = closeButtonRect(lb);
    g_pHyprOpenGL->renderRect(pxb(br, s),
                              cfg::colors.close_button.get(e),
                              {.round = pxr(br.h / 2.0, s)});
    if (m_closeGlyph && m_closeGlyph->m_size.x > 0) {
      const double gw = m_closeGlyph->m_size.x, gh = m_closeGlyph->m_size.y;
      const double gs = std::min((br.w * 0.62) / std::max(1.0, gw),
                                 (br.h * 0.62) / std::max(1.0, gh));
      const double dw = gw * gs, dh = gh * gs;
      g_pHyprOpenGL->renderTexture(
          m_closeGlyph,
          pxb(CBox(br.x + (br.w - dw) / 2.0, br.y + (br.h - dh) / 2.0, dw, dh), s),
          {.a = static_cast<float>(e)});
    }
  }
  glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
  if (!blendBefore)
    glDisable(GL_BLEND);
}

// Z1: the non-dragged tiles' live content, immediately after ALL tile chrome
// (and after ghosts, which the painter draws first). Previews render fully
// OPAQUE except the ws_enter_anim == "fade" entry (entryFade): at progress 0
// currentBox == natural == the real window's settled geometry, so the opaque
// preview overlays it pixel-perfect — fading would flicker the close tail.
// Population is otherwise expressed by the box scale/translate in
// currentBox() alone.
void Overview::renderMainWindows() const {
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const int    dragIdx = draggedTile();
  const double scale   = m->m_scale;
  const auto   when    = Time::steadyNow();
  const int    round   = pxr(cfg::look.preview_round, scale);
  const float  roundPow = cfg::look.preview_round_power;
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    if (static_cast<int>(i) == dragIdx)
      continue;
    const auto w = m_tiles[i].win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;
    if (swapfxActive(w))
      continue; // flying via a landing (Z2.5) — suppressed here
    const LRect lb = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
    const CBox  px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
    renderWindowLive(w, m, px, px,
                     static_cast<float>(entryFade(i)), when, round, roundPow);
  }
}

// Removed-by-rebuild tiles fading/scaling out where they were (all→one,
// close-window). Same rebuild clock as Tile.appear — the mirror direction.
// Softer than the incoming pop: ghosts are a motion cue, not the main event.
void Overview::renderGhosts() const {
  if (m_ghosts.empty() || m_rebuildClock.done(m_ghost.ms))
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double scale = m->m_scale;
  const auto when    = Time::steadyNow();
  const int round    = pxr(cfg::look.preview_round, scale);
  const float roundPow = cfg::look.preview_round_power;
  const double p    = std::min(1.0, m_rebuildClock.raw(m_ghost.ms));
  const double eOut = curves::eval(m_ghost.curve, p); // 0..1 gone

  for (const auto &g : m_ghosts) {
    const auto w = g.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;
    // The exit mirrors the entry's pop-in vividly: scale 1 -> 0.7, alpha
    // 0.85 -> 0, over the FULL window. A slide exit moves the ghosts OUT to
    // the opposite side / through the top edge instead of shrinking them.
    const double k  = 1.0 - 0.3 * eOut; // shrink toward its own center
    const double cx = g.box.x + g.box.w / 2.0, cy = g.box.y + g.box.h / 2.0;
    const LRect box{cx - g.box.w * k / 2.0, cy - g.box.h * k / 2.0,
                    g.box.w * k, g.box.h * k};
    double gx = box.x, gy = box.y;
    float gAlpha = static_cast<float>((1.0 - eOut) * 0.85);
    if (m_wsSlideDir != 0 && m_exitStyle == "slide") {
      gx -= static_cast<double>(m_wsSlideDir) * m->m_size.x * eOut;
    } else if (m_wsSlideDir != 0 && m_exitStyle == "slidevert") {
      gy -= static_cast<double>(m->m_size.y) * eOut;
    }
    const CBox px(gx * scale, gy * scale, box.w * scale, box.h * scale);
    renderWindowLive(w, m, px, px, gAlpha, when, round, roundPow);
  }
}

} // namespace gloview
