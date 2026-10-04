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

bool Overview::closeButtonsAlwaysOn() const {
  return cfg::look.close_button_visibility.get() == "always";
}

bool Overview::closeOnDoubleClick() const {
  return cfg::look.close_trigger.get() == "doubleclick";
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

double Overview::entryFade(size_t i) const {
  if (i >= m_tiles.size())
    return 1.0;
  const auto &t = m_tiles[i];
  if (t.appear >= 1.0 || m_wsSlideDir == 0 || m_enterStyle != "fade")
    return 1.0;
  return tileAppear(static_cast<int>(i));
}

void Overview::drawPreviewTile(size_t i, const LRect &slot, bool lift) const {
  drawPreviewChrome(i, tileContentBox(i, slot), lift);
}

void Overview::drawPreviewChrome(size_t i, const LRect &slot, bool lift,
                                 float alphaMul) const {
  const auto m = m_monitor.lock();
  if (!m || i >= m_tiles.size())
    return;
  const double s        = m->m_scale;
  const double e        = eased();
  const float  f        = static_cast<float>(entryFade(i)) * alphaMul;
  const int    round    = cfg::look.preview_round;
  const float  roundPow = cfg::look.preview_round_power;
  const auto shadowCol  = cfg::colors.shadow.get(1.0);
  const auto hoverCol   = cfg::colors.hover_border.get(e * f);

  const auto &t = m_tiles[i];
  const auto w  = t.win.lock();
  if (!w || !w->m_isMapped || w->isHidden())
    return;

  const LRect lb = slot;

  dropShadow(lb, s, shadowCol, lift ? 14.0 : 6.0, lift ? 30.0 : 16.0,
             e * 0.18F * f, round, roundPow);

  const bool framed   = (static_cast<int>(i) == m_hovered || lift);
  const bool selected = (static_cast<int>(i) == m_selected) && !lift;

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

  // Transition frost: the cached blur redrawn at CONSTANT alpha (fading it
  // leaves C*e*(1-e) of the sharp desktop leaking), plus the dim rect at the
  // GLOBAL alpha e so the interior's dim tracks the surroundings. Rounded
  // like the content, UV-mapped to the tile's region. Stops at e >= 0.999.
  // alphaMul is a per-card dissolve, not the global dim, so it scales both.
  const double apNow =
      t.appear < 1.0 ? tileAppear(static_cast<int>(i)) : 1.0;
  if (e < 0.999 || apNow < 0.999 || alphaMul < 0.999) {
    if (const auto btex = backdropBlurTexture(); btex && btex->ok()) {
      const double pxW = static_cast<double>(m->m_pixelSize.x);
      const double pxH = static_cast<double>(m->m_pixelSize.y);
      const CBox  lbPx = pxb(lb, s);
      CTexPassElement::SRenderData td{};
      td.tex           = btex;
      td.box           = lbPx;
      td.a             = alphaMul;
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
      // The frost covers the global dim: re-apply it at the global alpha.
      g_pHyprOpenGL->renderRect(
          lbPx,
          cfg::blur.backdrop.get(static_cast<float>(e) * alphaMul),
          {.round = pxr(round, s), .roundingPower = roundPow});
    }
  }
  safetyBacking(lb, s, cfg::colors.backing.get(), 0.08 * alphaMul, round,
                roundPow);

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

void Overview::renderPreviews() const {
  const int dragIdx = draggedTile();
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    if (static_cast<int>(i) == dragIdx)
      continue; // the dragged tile floats over the strip; drawn later
    drawPreviewTile(i, currentBox(m_tiles[i], static_cast<int>(i)), false);
  }
}

void Overview::drawCloseButton(const LRect &lb, float a) const {
  const auto m = m_monitor.lock();
  if (!m || !m_closeGlyph || m_closeGlyph->m_size.x <= 0)
    return;
  const LRect br = closeButtonRect(lb);
  const double s = m->m_scale;
  g_pHyprOpenGL->renderRect(pxb(br, s), cfg::colors.close_button.get(a),
                            {.round = pxr(br.h / 2.0, s)});
  const double gw = m_closeGlyph->m_size.x, gh = m_closeGlyph->m_size.y;
  const double gs =
      std::min((br.w * 0.62) / std::max(1.0, gw), (br.h * 0.62) / std::max(1.0, gh));
  g_pHyprOpenGL->renderTexture(
      m_closeGlyph,
      pxb(LRect{br.x + (br.w - gw * gs) / 2.0, br.y + (br.h - gh * gs) / 2.0,
                gw * gs, gh * gs}, s),
      {.a = a});
}

// Per-window "✕": drawn AFTER the content, or opaque surfaces paint over it.
void Overview::renderTileButtons() const {
  if (closeOnDoubleClick())
    return;
  if (!m_desktopMode && !closeButtonsAlwaysOn())
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  // Defensive: GL_BLEND left off by a previous pass makes the glyph's
  // transparent padding write through as opaque black; restore the exact func.
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
  const int    dragIdx = draggedTile();
  const float  a       = static_cast<float>(eased());
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    if (static_cast<int>(i) == dragIdx)
      continue;
    const auto w = m_tiles[i].win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;
    drawCloseButton(
        tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i))), a);
  }
  glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
  if (!blendBefore)
    glDisable(GL_BLEND);
}

// Previews render fully OPAQUE except the ws_enter fade: at progress 0
// currentBox == natural, so the opaque preview covers the real window exactly.
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
    const CBox  px = pxb(lb, scale);
    const bool settled = m_tileClock.done(glideDur());
    renderWindowLive(w, m, px, px,
                     static_cast<float>(entryFade(i)), when, round, roundPow,
                     settled);
  }
}

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
    const double k  = 1.0 - 0.3 * eOut; // shrink toward its own center
    const double cx = g.box.x + g.box.w / 2.0, cy = g.box.y + g.box.h / 2.0;
    const LRect box{cx - g.box.w * k / 2.0, cy - g.box.h * k / 2.0,
                    g.box.w * k, g.box.h * k};
    LRect slidBox = box;
    float gAlpha = static_cast<float>((1.0 - eOut) * 0.85);
    if (m_wsSlideDir != 0 && m_exitStyle == "slide") {
      slidBox.x -= static_cast<double>(m_wsSlideDir) * m->m_size.x * eOut;
    } else if (m_wsSlideDir != 0 && m_exitStyle == "slidevert") {
      slidBox.y -= static_cast<double>(m->m_size.y) * eOut;
    }
    const CBox px = pxb(slidBox, scale);
    renderWindowLive(w, m, px, px, gAlpha, when, round, roundPow);
  }
}

} // namespace gloview
