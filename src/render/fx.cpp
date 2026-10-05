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
#include "tile_look.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"
#include "../anim/curves.hpp"
#include "../debug/log.hpp"
#include "window_content.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {
// Down-right of the pointer, so the drop zones stay visible under the preview.
constexpr double offX = 46.0, offY = 64.0;
double dragScale() {
  return std::clamp(static_cast<double>(cfg::look.drag_size), 0.15, 1.0);
}
}

LRect Overview::dragBox() const {
  const int dragIdx = draggedTile();
  if (dragIdx < 0)
    return LRect{0, 0, 0, 0};
  const LRect base = m_tiles[dragIdx].target;
  if (m_desktopMode)
    return LRect{m_drag.x - m_drag.grabDX, m_drag.y - m_drag.grabDY,
                 base.w, base.h};
  const LRect full = tileBox(static_cast<size_t>(dragIdx), base);
  const double k   = dragScale();
  return LRect{m_drag.x + offX, m_drag.y + offY, full.w * k, full.h * k};
}

LRect Overview::dragStripBox() const {
  if (m_drag.win.expired())
    return LRect{0, 0, 0, 0};
  const double sw    = m_drag.fromBox.w > 1.0 ? m_drag.fromBox.w : 150.0;
  const double ww    = sw * dragScale();
  const auto size    = m_drag.win.lock()->sizeAnimation()->goal();
  const double aspect = (size.x > 0 && size.y > 0) ? size.x / size.y : 16.0 / 9.0;
  return LRect{m_drag.x + offX, m_drag.y + offY, ww, ww / std::max(0.1, aspect)};
}

double Overview::dragLiftProgress() const {
  return curves::eval(m_lift.curve, m_dragLiftClock.raw(m_lift.ms));
}

LRect Overview::dragVisualBox() const {
  const LRect to = m_drag.press == model::Drag::Press::Tile ? dragBox()
                                                               : dragStripBox();
  const double p = dragLiftProgress();
  LRect b = (p < 1.0 && m_drag.lifted) ? lerp(m_drag.fromBox, to, p) : to;

  // Scale/raise apply unconditionally: a stationary hold (fromBox == to) must
  // still animate.
  const double s  = 1.0 + 0.07 * p;
  const double dy = -5.0 * p;
  const double cx = b.cx(), cy = b.cy();
  return LRect{cx - b.w * s / 2.0, cy - b.h * s / 2.0 + dy, b.w * s, b.h * s};
}

// A dragged tile is the same tile, lifted: taller shadow, framed, no pill.
void Overview::renderDragTile() const {
  const int dragIdx = draggedTile();
  if (dragIdx < 0) {
    // A window dragged OUT of the strip has no tile; it gets its own chrome.
    if (m_drag.press == model::Drag::Press::StripWin && !m_drag.win.expired())
      drawDragStripChrome();
    return;
  }
  TileLook look;
  look.box   = dragVisualBox();
  look.lift  = true;
  look.round = clampRound(cfg::look.preview_round, look.box.w, look.box.h);
  drawTile(static_cast<size_t>(dragIdx), look);
}

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

  // A stroke, not a filled underlay: a fill shows through the window's own
  // transparency as a solid wash instead of a frame.
  const int th = cfg::look.hover_border_size; // 0 = no ring
  const Config::CGradientValueData grad(hoverCol);
  g_pHyprOpenGL->renderBorder(pxb(lb, s), grad,
                              {.round = pxr(round, s),
                               .roundingPower = roundPow,
                               .borderSize = th,
                               .a = 1.0F,
                               .outerRound = outerRoundPx(round, th, roundPow, s)});
}

void Overview::renderDragWindow() const {
  if (draggedTile() >= 0)
    return; // drawn whole by renderDragTile
  const auto m = m_monitor.lock();
  const auto w = m_drag.win.lock();
  if (!m || !w || !w->m_isMapped || w->isHidden())
    return;
  if (m_drag.press != model::Drag::Press::StripWin)
    return;

  const double scale = m->m_scale;
  const float  roundPow = cfg::look.preview_round_power;
  const LRect  lb    = dragVisualBox();
  const int    round = clampRound(cfg::look.preview_round, lb.w, lb.h);
  const CBox   px    = pxb(lb, scale);
  renderWindowLive(w, m, px, px, static_cast<float>(eased()), Time::steadyNow(),
                   contentRadiusPx(round, cfg::look.hover_border_size, roundPow,
                                   scale),
                   roundPow);
}

// The zone's affordance: the bar itself glowing, and a halo bleeding off its
// inner edge into the screen.
//
// Brightness tracks how close the CURSOR is (m_killGlow, a smoothstepped
// distance), so it guides the drop instead of switching on once you are already
// on top of it. renderRect takes a flat colour only, so everything is stepped at
// one pixel — a dozen fat bands read as stripes.
//
// No stacked rounded rectangles: concentric layers have visible edges of their
// own and read as geometry, not light. Both parts are a single per-pixel ramp
// whose alpha reaches ZERO at the far end, so there is no silhouette to see.
void Overview::renderKillZone() const {
  const auto m = m_monitor.lock();
  if (!m || m_killGlow <= 0.004)
    return;
  const LRect z = killZoneRect();
  if (z.w < 1.0 || z.h < 1.0)
    return;

  const double s    = m->m_scale;
  const double glow = m_killGlow;
  const auto   col  = cfg::colors.select_border.get(eased());
  // Strip at the bottom puts the zone at the top of the screen, and vice versa.
  const bool zoneAtTop = stripAnchor() == Anchor::Bottom;
  // ONE continuous falloff measured from the SCREEN edge, through the band and
  // on into the screen. Two ramps — a band and a halo — put a step where they
  // met: the band's inner end faded to nothing while the halo's first row was
  // already at 0.39, and that step read as a second line across the bottom.
  // A single ramp cannot have a seam.
  constexpr int kReach = 160; // 200 * 0.8: the glow is 20% shorter
  for (int d = 0; d < kReach; ++d) {
    const double f = static_cast<double>(d) / kReach;
    const double a = std::pow(1.0 - f, 2.5) * 0.4675 * glow; // 0.55 less 15%
    if (a < 0.004)
      continue;
    const double y = zoneAtTop ? z.y + d : z.y + z.h - 1 - d;
    g_pHyprOpenGL->renderRect(pxb(LRect{0.0, y, z.w, 1.0}, s), argb(col, a), {});
  }
}

void Overview::kickPulse(const PHLWINDOW &w) {
  if (!w || !leaf("pulse").on)
    return;
  std::erase_if(m_pulses, [&w](const model::WinPulse &p) { return p.w.lock() == w; });
  m_pulses.push_back(model::WinPulse{w, 0.0, std::chrono::steady_clock::now()});
}

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
            tileBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        // The pulse tracks the tile's own radius, so it lands on the same arc.
        drawPulseRing(pxb(lb, s),
                      pxr(clampRound(cfg::look.preview_round, lb.w, lb.h), s),
                      roundPow, col, pr);
        break;
      }
    }
  }
}

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

// Flying windows, lerped to the CURRENT slot box (which may itself be
// gliding, so the flight bends toward a moving destination). Slot rendering is
// suppressed while the flight is live and takes over invisibly at t=1.
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
    LRect to;
    bool found = false;
    for (size_t i = 0; i < m_tiles.size() && !found; ++i)
      if (m_tiles[i].win.lock() == w) {
        to = tileBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
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
    bool ring = false;

    if (fx.style == model::SwapStyle::SlideVert) {
      constexpr double LIFT = 12.0;
      if (t < 0.5) {
        const double q = curves::eval(fx.curve, t / 0.5);
        bx = fx.from.x;
        by = fx.from.y - (fx.from.h + LIFT) * q;
        bw = fx.from.w;
        bh = fx.from.h;
      } else {
        const double q = curves::eval(fx.curve, (t - 0.5) / 0.5);
        bx = to.x;
        by = to.y - (to.h + LIFT) * (1.0 - q);
        bw = to.w;
        bh = to.h;
        alpha = static_cast<float>(std::min(1.0, q * 2.0));
      }
    } else if (fx.style == model::SwapStyle::Fade) {
      if (t < 0.5) {
        bx = fx.from.x; by = fx.from.y; bw = fx.from.w; bh = fx.from.h;
        alpha = static_cast<float>(1.0 - t / 0.5);
      } else {
        bx = to.x; by = to.y; bw = to.w; bh = to.h;
        alpha = static_cast<float>((t - 0.5) / 0.5);
      }
    } else if (fx.style == model::SwapStyle::Pop) {
      if (t < 0.3) {
        bx = fx.from.x; by = fx.from.y; bw = fx.from.w; bh = fx.from.h;
        alpha = static_cast<float>(1.0 - t / 0.3);
      } else {
        const double q = (t - 0.3) / 0.7;
        const double k = 0.7 + 0.3 * curves::eval(leaf("card").curve, q);
        bx = to.x + to.w * (1.0 - k) / 2.0;
        by = to.y + to.h * (1.0 - k) / 2.0;
        bw = to.w * k;
        bh = to.h * k;
        alpha = static_cast<float>(std::min(1.0, q * 3.0));
      }
    } else {
      bx = lerp(fx.from.x, to.x, p);
      by = lerp(fx.from.y, to.y, p);
      bw = lerp(fx.from.w, to.w, p);
      bh = lerp(fx.from.h, to.h, p);
      ring = true;
    }

    const CBox px = pxb(LRect{bx, by, bw, bh}, s);
    const int rpx = pxr(clampRound(round, bw, bh), s);
    if (alpha > 0.02F)
      g_pHyprOpenGL->renderRoundedShadow(
          pxb(LRect{bx, by + 6.0, bw, bh}, s), rpx, roundPow,
          static_cast<int>(16.0 * s),
          Config::CGradientValueData(argb(cfg::colors.shadow.get(), 1.0)),
          static_cast<float>(0.18 * alpha));
    if (ring && t < 0.6)
      strokeRingPx(px, argb(cfg::colors.hover_border.get(), (1.0F - t / 0.6F) * 0.9F),
                   (1.0F - t / 0.6F) * 0.9F, rpx, roundPow);
    renderWindowLive(w, m, px, px, alpha, when,
                     contentRadiusPx(round, cfg::look.border_size, roundPow, s),
                     roundPow, true);
  }
}

bool Overview::isAboveLayer(const std::string &ns) const {
  if (ns.find("aboveoverview") != std::string::npos)
    return true;
  const std::string list = cfg::layer.above_namespaces.get();
  size_t i = 0;
  while (i < list.size()) {
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

// renderLayer is renderer-protected, so opted-in TOP/OVERLAY surfaces go
// through the immediate leaf like window content; damageBox forces a composite
// when the client itself has no damage.
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

      const auto pos = ls->positionAnimation()->value(); // absolute logical
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

}
