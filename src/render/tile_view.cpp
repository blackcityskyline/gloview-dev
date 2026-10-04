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
#include "tile_look.hpp"
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

bool Overview::closeButtonsAlwaysOn() const {
  return cfg::look.close_button_visibility.get() == "always";
}

bool Overview::closeOnDoubleClick() const {
  return cfg::look.close_trigger.get() == "doubleclick";
}

// One rule for every layout, and the hit-test reads this same function, so the
// ✕ can never appear where a click will not close.
bool Overview::tileCloseButtons() const {
  return !closeOnDoubleClick() && (m_desktopMode || closeButtonsAlwaysOn());
}

LRect Overview::closeButtonRect(const LRect &lb) const {
  const double scale = std::max(0.3, static_cast<double>(
                                         cfg::look.close_button_size));
  const double r     = std::clamp(std::min(lb.w, lb.h) * 0.11, 9.0, 18.0) * scale;
  const double inset = r + 6.0;
  const std::string pos = cfg::look.close_button_position.get();
  double cx = lb.x + lb.w - inset, cy = lb.y + inset;
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

double Overview::entryFade(size_t i) const {
  if (i >= m_tiles.size())
    return 1.0;
  const auto &t = m_tiles[i];
  if (t.appear >= 1.0 || m_wsSlideDir == 0 || m_enterStyle != "fade")
    return 1.0;
  return tileAppear(static_cast<int>(i));
}

// A preview always wears the window's own aspect: the surface covers its box
// from the centre, so only a matching aspect lets the content's corner arcs meet
// the ring's. Every layout resolves its slot through here, and so does every
// hit-test — a second way to compute a tile box is a second way to be wrong.
LRect Overview::tileBox(size_t i, const LRect &slot) const {
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

// THE tile. Shadow, ring, focus ring, transition frost, safety backing, the
// content, the pill and the ✕ — in this order, each layer under the next, for
// every layout. A layout contributes a box and flags and nothing else; the ring
// width that goes into contentRadiusPx is the one actually stroked below, which
// is why the content cannot come out with corners of a different shape.
void Overview::drawTile(size_t i, const TileLook &look) const {
  const auto m = m_monitor.lock();
  if (!m || i >= m_tiles.size())
    return;
  const auto &t = m_tiles[i];
  const auto w  = t.win.lock();
  if (!w || !w->m_isMapped || w->isHidden())
    return;

  const LRect box = look.box;
  if (box.w < 1.0 || box.h < 1.0)
    return;

  const double s        = m->m_scale;
  const double e        = eased();
  const float  f        = static_cast<float>(entryFade(i));
  const int    round    = look.round >= 0.0
                              ? static_cast<int>(look.round)
                              : cfg::look.preview_round;
  const float  roundPow = cfg::look.preview_round_power;
  const auto shadowCol  = cfg::colors.shadow.get(1.0);

  const bool framed   = static_cast<int>(i) == m_hovered || look.lift;
  const bool selected = static_cast<int>(i) == m_selected && !look.lift;
  const bool focused  = cfg::look.show_focus_border != 0 && (framed || selected);
  // Whichever ring ends up on top is the one the content has to tuck under.
  const int ringW = focused ? (framed ? cfg::look.hover_border_size
                                      : cfg::look.select_border_size)
                            : cfg::look.border_size;

  dropShadow(box, s, shadowCol, look.lift ? 14.0 : 6.0, look.lift ? 30.0 : 16.0,
             static_cast<float>(e * 0.18 * f), round, roundPow);

  if (cfg::look.show_border != 0)
    strokeRing(box, s, cfg::colors.border.get(e * f), cfg::look.border_size,
               round, roundPow);
  if (focused)
    strokeRing(box, s,
               framed ? cfg::colors.hover_border.get(e * f)
                      : cfg::colors.select_border.get(e * f),
               ringW, round, roundPow);

  // Transition frost: the cached blur redrawn at CONSTANT alpha (fading it
  // leaves C*e*(1-e) of the sharp desktop leaking), plus the dim rect at the
  // GLOBAL alpha e so the interior's dim tracks the surroundings. Rounded like
  // the content, UV-mapped to the tile's region. Stops at e >= 0.999.
  const double apNow = t.appear < 1.0 ? tileAppear(static_cast<int>(i)) : 1.0;
  if (e < 0.999 || apNow < 0.999) {
    if (const auto btex = backdropBlurTexture(); btex && btex->ok()) {
      const double pxW = static_cast<double>(m->m_pixelSize.x);
      const double pxH = static_cast<double>(m->m_pixelSize.y);
      const CBox  boxPx = pxb(box, s);
      CTexPassElement::SRenderData td{};
      td.tex           = btex;
      td.box           = boxPx;
      td.a             = 1.0F;
      td.round         = pxr(round, s);
      td.roundingPower = roundPow;
      td.allowCustomUV = true;
      auto &uvTL = g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft;
      auto &uvBR = g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight;
      uvTL = Vector2D{boxPx.x / pxW, boxPx.y / pxH};
      uvBR = Vector2D{(boxPx.x + boxPx.width) / pxW,
                      (boxPx.y + boxPx.height) / pxH};
      g_pHyprRenderer->draw(td, g_pHyprRenderer->m_renderData.damage);
      uvTL = Vector2D{-1.0, -1.0};
      uvBR = Vector2D{-1.0, -1.0};
      // The frost covers the global dim: re-apply it at the global alpha.
      g_pHyprOpenGL->renderRect(boxPx, cfg::blur.backdrop.get(static_cast<float>(e)),
                                {.round = pxr(round, s), .roundingPower = roundPow});
    }
  }
  // A card is filled edge to edge: its content is contained, so whatever the
  // window does not cover would otherwise be bare backdrop inside the ring.
  if (look.card) {
    const CBox boxPx = pxb(box, s);
    if (boxPx.width > 0 && boxPx.height > 0)
      g_pHyprOpenGL->renderRect(boxPx, cfg::colors.backing.get(e * f),
                                {.round = pxr(round, s), .roundingPower = roundPow});
  } else {
    safetyBacking(box, s, cfg::colors.backing.get(), 0.08, round, roundPow);
  }

  renderWindowLive(w, m, pxb(box, s), pxb(box, s), f, Time::steadyNow(),
                   contentRadiusPx(round, ringW, roundPow, s, !look.card),
                   roundPow, m_tileClock.done(glideDur()), look.card);

  if (look.tone > 0.0)
    g_pHyprOpenGL->renderRect(pxb(box, s), argb(shadowCol, look.tone * e * f),
                              {.round = pxr(round, s), .roundingPower = roundPow});

  if ((framed || selected) && !look.lift && t.label && t.label->m_size.x > 0) {
    const double lw   = t.label->m_size.x;
    const double lh   = t.label->m_size.y;
    const double padX = 14.0, padY = 6.0;
    const double pw   = lw + 2 * padX;
    const double ph   = lh + 2 * padY;
    const double px   = std::clamp(box.cx() - pw / 2.0, 6.0, m->m_size.x - pw - 6.0);
    const double py   = std::min(box.y + box.h + 10.0, m->m_size.y - ph - 6.0);
    g_pHyprOpenGL->renderRect(pxb(CBox(px, py, pw, ph), s),
                              cfg::colors.title_pill.get(e * f),
                              {.round = pxr(ph / 2.0, s)});
    g_pHyprOpenGL->renderTexture(t.label, pxb(CBox(px + padX, py + padY, lw, lh), s),
                                 {.a = static_cast<float>(e * f)});
  }

  if (look.button && !look.lift && m_closeGlyph && m_closeGlyph->m_size.x > 0) {
    const LRect br = closeButtonRect(box);
    g_pHyprOpenGL->renderRect(pxb(br, s), cfg::colors.close_button.get(e),
                              {.round = pxr(br.h / 2.0, s)});
    const double gw = m_closeGlyph->m_size.x, gh = m_closeGlyph->m_size.y;
    const double gs = std::min((br.w * 0.62) / std::max(1.0, gw),
                               (br.h * 0.62) / std::max(1.0, gh));
    g_pHyprOpenGL->renderTexture(
        m_closeGlyph,
        pxb(LRect{br.x + (br.w - gw * gs) / 2.0, br.y + (br.h - gh * gs) / 2.0,
                  gw * gs, gh * gs}, s),
        {.a = static_cast<float>(e)});
  }
}

void Overview::renderTiles() const {
  const auto m = m_monitor.lock();
  if (!m)
    return;
  // Defensive: GL_BLEND left off by a previous pass makes the ✕ glyph's
  // transparent padding write through as opaque black; restore the exact func.
  const bool wantButtons = tileCloseButtons();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  const bool blendBefore = glIsEnabled(GL_BLEND) == GL_TRUE;
  GLint srcRGB = GL_ONE, dstRGB = GL_ZERO, srcA = GL_ONE, dstA = GL_ZERO;
  glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
  glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  const int dragIdx = draggedTile();
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    if (static_cast<int>(i) == dragIdx)
      continue; // the dragged tile floats over the strip; drawn later
    const auto w = m_tiles[i].win.lock();
    if (!w || !w->m_isMapped || w->isHidden() || swapfxActive(w))
      continue; // flying via a landing: its slot is painted by renderSwapFX
    TileLook look;
    look.box = tileBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
    look.button = wantButtons;
    drawTile(i, look);
  }

  glBlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
  if (!blendBefore)
    glDisable(GL_BLEND);
}

void Overview::renderGhosts() const {
  if (m_ghosts.empty() || m_rebuildClock.done(m_ghost.ms))
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double scale = m->m_scale;
  const auto when    = Time::steadyNow();
  const float roundPow = cfg::look.preview_round_power;
  const double p    = std::min(1.0, m_rebuildClock.raw(m_ghost.ms));
  const double eOut = curves::eval(m_ghost.curve, p); // 0..1 gone

  for (const auto &g : m_ghosts) {
    const auto w = g.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;
    const double k  = 1.0 - 0.3 * eOut; // shrink toward its own center
    const double cx = g.box.x + g.box.w / 2.0, cy = g.box.y + g.box.h / 2.0;
    const LRect box{cx - g.box.w * k / 2.0, cy - g.box.h * k / 2.0,
                    g.box.w * k, g.box.h * k};
    LRect slidBox = box;
    float gAlpha = static_cast<float>((1.0 - eOut) * 0.85);
    if (m_wsSlideDir != 0 && m_exitStyle == "slide") {
      slidBox.x -= static_cast<double>(m_wsSlideDir) * m->m_size.x * eOut;
    } else if (m_wsSlideDir != 0 && m_exitStyle == "slidevert") {
      slidBox.y -= static_cast<double>(m_wsSlideDir) * m->m_size.y * eOut;
    }
    const CBox px = pxb(slidBox, scale);
    renderWindowLive(w, m, px, px, gAlpha, when,
                     contentRadiusPx(cfg::look.preview_round,
                                     cfg::look.border_size, roundPow, scale),
                     roundPow, true);
  }
}

} // namespace gloview
