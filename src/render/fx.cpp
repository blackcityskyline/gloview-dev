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

#include "../debug/log.hpp"
#include "gl_util.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"
#include "../anim/curves.hpp"
#include "window_content.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {
double lerp(double a, double b, double t) { return a + (b - a) * t; }
} // namespace

// ---- drag visuals (Z3) ------------------------------------------------------

// The picked-up grid tile's floating box. Grid mode shrinks to half AND sits
// offset down-right of the cursor — a preview under the pointer would hide
// the drop-zone indicator it's meant to be aimed at. Desktop/canvas mode
// keeps the true 1:1 grab offset and full size: there a drop PARKS the window
// at exactly that visual spot.
LRect Overview::dragBox() const {
  const int dragIdx = draggedTile();
  if (dragIdx < 0)
    return LRect{0, 0, 0, 0};
  const LRect base = m_tiles[dragIdx].target;
  if (m_desktopMode)
    return LRect{m_drag.x - m_drag.grabDX, m_drag.y - m_drag.grabDY,
                 base.w, base.h};
  const double w = base.w * 0.5;
  const double h = base.h * 0.5;
  constexpr double offX = 46.0, offY = 64.0;
  return LRect{m_drag.x + offX, m_drag.y + offY, w, h};
}

// The picked-up strip window's floating box: small AND offset down-right of
// the cursor, same reason as dragBox.
LRect Overview::dragStripBox() const {
  const auto w = m_drag.win.lock();
  if (!w)
    return LRect{0, 0, 0, 0};
  const auto size   = w->sizeAnimation()->goal();
  const double aspect = (size.x > 0 && size.y > 0) ? size.x / size.y : 16.0 / 9.0;
  const double w_   = 150.0; // fixed on-screen preview width while dragging
  const double h_   = w_ / std::max(0.1, aspect);
  constexpr double offX = 46.0, offY = 64.0;
  return LRect{m_drag.x + offX, m_drag.y + offY, w_, h_};
}

// Z3: chrome for whichever drag is live (grid tile or strip window).
void Overview::renderDragTile() const {
  const int dragIdx = draggedTile();
  if (dragIdx >= 0) {
    drawPreviewTile(static_cast<size_t>(dragIdx), dragBox(), true); // chrome; content right after
    return;
  }
  if (m_drag.press == model::Drag::Press::StripWin && !m_drag.win.expired())
    drawDragStripChrome();
}

// Chrome for a window dragged straight off the strip — same visual language
// as a grid-tile drag (drawPreviewTile), scaled down to match.
void Overview::drawDragStripChrome() const {
  const auto m = m_monitor.lock();
  const auto w = m_drag.win.lock();
  if (!m || !w)
    return;
  const double s        = m->m_scale;
  const double e        = eased();
  const LRect  lb       = dragStripBox();
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

// Z3: the dragged window's live content, right after its chrome.
void Overview::renderDragWindow() const {
  const int dragIdx = draggedTile();
  const auto m = m_monitor.lock();
  if (!m)
    return;
  if (dragIdx >= 0) {
    const auto w = m_tiles[dragIdx].win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      return;
    const double e     = eased();
    const double scale = m->m_scale;
    const LRect  lb    = tileContentBox(static_cast<size_t>(dragIdx), dragBox());
    const CBox   px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
    const int    round = pxr(cfg::look.preview_round, scale);
    renderWindowLive(w, m, px, px, static_cast<float>(e), Time::steadyNow(),
                     round, cfg::look.preview_round_power);
    return;
  }
  if (m_drag.press == model::Drag::Press::StripWin) {
    const auto w = m_drag.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      return;
    const double e     = eased();
    const double scale = m->m_scale;
    const LRect  lb    = dragStripBox();
    const CBox   px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
    const int round = pxr(clampRound(cfg::look.preview_round,
                                     lb.w, lb.h),
                          scale);
    renderWindowLive(w, m, px, px, static_cast<float>(e), Time::steadyNow(),
                     round, cfg::look.preview_round_power);
  }
}

// ---- swap pulses (Z1 tail for grid, Z2 tail for strip) ----------------------

void Overview::kickPulse(const PHLWINDOW &w) {
  if (!w || !anim("swap_pulse").on)
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
void Overview::renderLandings() const {
  if (m_landings.empty())
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double s     = m->m_scale;
  const auto when    = Time::steadyNow();
  const int  round   = cfg::look.preview_round;
  const float roundPow = cfg::look.preview_round_power;
  const double dur = dropDur();
  for (const auto &l : m_landings) {
    const auto w = l.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;
    if (!l.dbgLogged) {
      l.dbgLogged = true;
      debug::dbg("landing FRAME0 win alive, drawing flight");
    }
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
    const double p  = curves::eval(anim("drop").curve, l.clock.raw(dur));
    const double bx = l.from.x + (to.x - l.from.x) * p;
    const double by = l.from.y + (to.y - l.from.y) * p;
    const double bw = l.from.w + (to.w - l.from.w) * p;
    const double bh = l.from.h + (to.h - l.from.h) * p;
    const CBox px(bx * s, by * s, bw * s, bh * s);
    // Flight chrome — the drag preview carried a shadow and a ring; without
    // the same chrome here the release reads as "everything vanished, a bare
    // window snapped in". The ring fades out over the flight (the slot has
    // no permanent ring); the shadow holds, matching the drag preview.
    const float ringA = static_cast<float>((1.0 - p) * 0.9);
    if (ringA > 0.02F)
      strokeRingPx(px, argb(cfg::colors.hover_border.get(), ringA),
                   ringA, pxr(clampRound(round, bw, bh), s), roundPow);
    g_pHyprOpenGL->renderRoundedShadow(
        pxb(LRect{bx, by + 6.0, bw, bh}, s), pxr(clampRound(round, bw, bh), s),
        roundPow, static_cast<int>(16.0 * s),
        Config::CGradientValueData(argb(cfg::colors.shadow.get(), 1.0)),
        static_cast<float>(0.18 * (1.0 - p * 0.5)));
    renderWindowLive(w, m, px, px, 1.0F, when,
                     pxr(clampRound(round, bw, bh), s), roundPow);
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
