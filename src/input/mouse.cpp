#include "../config/config.hpp"
#include "../overview.hpp"
#include "../debug/log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {


bool Overview::onMouseAxis(const IPointer::SAxisEvent &e) {
  if (!m_active)
    return false;
  const double notches = e.deltaDiscrete != 0
                             ? static_cast<double>(e.deltaDiscrete)
                             : e.delta / 15.0;

  bool overStrip = false;
  if (const auto m = m_monitor.lock()) {
    const auto mc = g_pInputManager->getMouseCoordsInternal();
    const LRect band = stripBand();
    const double lx = mc.x - m->m_position.x;
    const double ly = mc.y - m->m_position.y;
    overStrip = band.contains(lx, ly);
  }

  // The carousel run pages on the wheel: notches * carousel_scroll_speed = cards
  // moved, so 1 is one card per notch, 0.5 one card every two notches, 3 three
  // per notch. Its own switch, not scroll_switches_workspace: that one is about
  // WORKSPACES, and gating the run on it meant a config with workspace scrolling
  // off got a wheel that did nothing at all.
  if (!overStrip && carouselMode()) {
    const double speed =
        std::clamp(static_cast<double>(cfg::carousel.scroll_speed), 0.05, 20.0);
    const double cards = notches * speed;
    // Below one card per notch the leftover is carried, so a slow setting still
    // moves, just a fraction of a card at a time instead of stalling.
    m_scrollAccum += cards;
    const int whole = static_cast<int>(std::trunc(m_scrollAccum));
    if (whole != 0) {
      m_scrollAccum -= static_cast<double>(whole);
      stepAltTab(whole);
    }
    return true;
  }

  if (!overStrip &&
      cfg::behavior.scroll_switches_workspace != 0) {
    if (notches != 0.0)
      stepWorkspace(notches > 0 ? 1 : -1);
    return true;
  }

  // Modal: swallow the wheel even with nothing to scroll.
  if (m_stripScrollMax <= 0.0)
    return true;
  const double step = stripThickness() * 0.9; // ~one card per notch
  animateStripTo(
      m_stripScroll,
      std::clamp(m_stripScrollTarget + notches * step, 0.0, m_stripScrollMax));
  updateHover();
  damage();
  return true;
}

void Overview::onMouseMove() {
  // Damage only the cursor's old+new boxes, never the whole monitor.
  const auto m = m_monitor.lock();
  if (!m || !Pointer::mgr()) {
    updateHover();
    return;
  }
  if (m_cursor.hasHardwareCursor(m)) {
    updateHover();
    return;
  }
  const CBox unionBox = m_cursor.moveDamage(m);
  updateHover();
  if (!m_active || unionBox.w <= 0 || unionBox.h <= 0)
    return;
  if (g_pHyprRenderer)
    g_pHyprRenderer->damageBox(unionBox);
}

int Overview::tileAt(double lx, double ly) const {
  if (carouselMode()) {
    // Cards overlap: first-match hits the wrong one, so scan front to back.
    int best = -1, bestDepth = 1 << 30;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
      if (i >= m_carousel.cards.size() || !m_carousel.cards[i].visible)
        continue;
      if (!currentBox(m_tiles[i], static_cast<int>(i)).contains(lx, ly))
        continue;
      if (m_carousel.cards[i].depth < bestDepth) {
        bestDepth = m_carousel.cards[i].depth;
        best = static_cast<int>(i);
      }
    }
    return best;
  }
  for (size_t i = 0; i < m_tiles.size(); ++i)
    if (currentBox(m_tiles[i], static_cast<int>(i)).contains(lx, ly))
      return static_cast<int>(i);
  return -1;
}

int Overview::stripItemAt(double lx, double ly) const {
  for (size_t i = 0; i < m_strip.size(); ++i)
    if (stripCardAt(i).contains(lx, ly))
      return static_cast<int>(i);
  return -1;
}

int Overview::draggedTile() const {
  return (m_drag.press == model::Drag::Press::Tile && m_drag.lifted &&
          m_drag.idx < static_cast<int>(m_tiles.size()))
             ? m_drag.idx
             : -1;
}

void Overview::updateHover() {
  if (!m_active)
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const auto mc = g_pInputManager->getMouseCoordsInternal();
  const double lx = mc.x - m->m_position.x;
  const double ly = mc.y - m->m_position.y;

  if (m_drag.armed()) {
    if (m_drag.lifted) {
      m_drag.x = lx;
      m_drag.y = ly;
      m_hoveredStrip = stripItemAt(lx, ly);
      m_hovered = (m_drag.press == model::Drag::Press::Tile)
                      ? m_drag.idx
                      : -1;
      damage();
      return;
    }
  }

  const int newTile = tileAt(lx, ly);
  const int newStrip = stripItemAt(lx, ly);
  if (newTile != m_hovered || newStrip != m_hoveredStrip) {
    const int oldTile = m_hovered, oldStrip = m_hoveredStrip,
              oldSel = m_selected;
    m_hovered = newTile;
    m_hoveredStrip = newStrip;
    // Carousel: hover must NOT move the selection (slots are built around it).
    if (newTile >= 0 && cfg::behavior.focus_follows_mouse != 0 &&
        !carouselMode()) {
      m_selected = newTile;
      syncFocus(); // so passthrough keybinds hit it
    }
    const auto m = m_monitor.lock();
    if (m) {
      auto dmgTile = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_tiles.size()))
          return;
        LRect b = currentBox(m_tiles[idx], idx);
        b.grow(48.0);
        g_pHyprRenderer->damageBox(
            CBox{b.x + m->m_position.x, b.y + m->m_position.y, b.w, b.h});
      };
      auto dmgCard = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_strip.size()))
          return;
        LRect b = stripCardAt(idx);
        b.grow(40.0);
        // absolute logical coords, no pxb scale
        g_pHyprRenderer->damageBox(
            CBox{b.x + m->m_position.x, b.y + m->m_position.y, b.w, b.h});
      };
      dmgTile(oldTile);
      dmgTile(newTile);
      if (oldSel != m_selected)
        dmgTile(oldSel);
      dmgCard(oldStrip);
      dmgCard(newStrip);
    }
  }
}

namespace {
// close_trigger=doubleclick: max gap; a touchpad double-tap is that pair.
constexpr auto DBLCLICK_WINDOW = std::chrono::milliseconds(400);
} // namespace

bool Overview::onMouseButton(const IPointer::SButtonEvent &e) {
  if (!m_active)
    return false;

  const auto m = m_monitor.lock();
  if (!m) {
    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
      close();
    return true;
  }
  const auto mc = g_pInputManager->getMouseCoordsInternal();
  const double lx = mc.x - m->m_position.x;
  const double ly = mc.y - m->m_position.y;

  if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
    m_drag = {};
    m_drag.press = model::Drag::Press::Empty;
    m_drag.button = e.button;

    if (e.button == BTN_MIDDLE) {
      const int idx = stripItemAt(lx, ly);
      if (idx >= 0 && m_strip[idx].kind != model::StripItem::Kind::Plus && m_strip[idx].kind != model::StripItem::Kind::All)
        closeWorkspaceWindows(m_strip[idx]);
      return true;
    }

    if (tileCloseButtons()) {
      // tileBox is what drawTile used, so the hit-test cannot drift from it.
      const int dragIdx = draggedTile();
      for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == dragIdx)
          continue;
        if (carouselMode() &&
            (i >= m_carousel.cards.size() || !m_carousel.cards[i].visible))
          continue; // off-run: nothing of it is drawn
        const LRect cur = currentBox(m_tiles[i], static_cast<int>(i));
        if (closeButtonRect(tileBox(i, cur)).contains(lx, ly)) {
          m_drag.press =
              model::Drag::Press::Consumed; // handled on press
          closeTileWindow(static_cast<int>(i));
          return true;
        }
      }
    }

    if (m_desktopMode || closeButtonsAlwaysOn()) {
      for (size_t i = 0; i < m_strip.size(); ++i) {
        if (m_strip[i].kind == model::StripItem::Kind::Plus || m_strip[i].kind == model::StripItem::Kind::All)
          continue;
        const LRect c = stripCardAt(i);
        const LRect br = closeButtonRect(c);
        if (br.contains(lx, ly)) {
          m_drag.press = model::Drag::Press::Consumed;
          closeWorkspaceWindows(m_strip[i]);
          return true;
        }
      }
    }

    for (size_t i = 0; i < m_strip.size(); ++i) {
      const LRect c = stripCardAt(i);
      if (!c.contains(lx, ly))
        continue;
      auto &it = m_strip[i];
      if (it.kind == model::StripItem::Kind::All) {
        m_drag.press = model::Drag::Press::Consumed;
        toggleAllWorkspaces();
      } else if (it.kind == model::StripItem::Kind::Plus) {
        m_drag.press = model::Drag::Press::Consumed;
        addWorkspace();
      } else {
        bool hitSlot = false;
        for (size_t j = 0; j < it.wins.size(); ++j) {
          const auto w = it.wins[j].win.lock();
          if (!w || !w->m_isMapped || w->isHidden())
            continue;
          const LRect wb = stripWinSlotRect(it, c, j);
          if (wb.contains(lx, ly)) {
            m_drag.press   = model::Drag::Press::StripWin;
            m_drag.idx     = static_cast<int>(i);
            m_drag.winIdx  = static_cast<int>(j);
            m_drag.win     = w;
            m_drag.pressX = m_drag.x = lx;
            m_drag.pressY = m_drag.y = ly;
            m_drag.grabDX = lx - wb.x;
            m_drag.grabDY = ly - wb.y;
            m_drag.fromBox = stripWinSlotRect(it, c, j);
            m_drag.holdStartMs = std::chrono::steady_clock::now();
            hitSlot = true;
            break;
          }
        }
        if (!hitSlot) {
          m_drag.press = model::Drag::Press::Consumed;
          switchToWorkspace(it);
        }
      }
      return true;
    }
    if (const int hit = tileAt(lx, ly); hit >= 0) {
      m_drag.press = model::Drag::Press::Tile;
      m_drag.idx = hit;
      const LRect b = currentBox(m_tiles[hit], hit);
      m_drag.pressX = m_drag.x = lx;
      m_drag.pressY = m_drag.y = ly;
      m_drag.grabDX = lx - b.x;
      m_drag.grabDY = ly - b.y;
      m_drag.fromBox = tileBox(static_cast<size_t>(hit), b);
      m_drag.holdStartMs = std::chrono::steady_clock::now();
      return true;
    }
    m_drag.press = model::Drag::Press::Empty;
    return true;
  }

  if (e.button == BTN_MIDDLE)
    return true; // middle was fully handled on press

  switch (m_drag.press) {
  case model::Drag::Press::Consumed: { // handled on press
    debug::dbg("[REL] press=Consumed");
    m_drag = {};
    return true;
  }
  case model::Drag::Press::Tile: {
    const int press = m_drag.idx;
    const auto w = m_tiles[press].win.lock();
    const double grabDX = m_drag.grabDX, grabDY = m_drag.grabDY;
    debug::dbg("[REL] press=Tile lifted=" + std::to_string(m_drag.lifted) +
               " rmb=" + std::to_string(m_drag.button == BTN_RIGHT) +
               " dxMoved=" + std::to_string(m_drag.travelSq()) +
               " idx=" + std::to_string(press) +
               " win=" + (w ? "valid" : "expired"));
    if (m_drag.lifted && m_drag.travelSq() < 64.0) {
      m_drag.lifted = false;
      m_dragLiftClock = anim::Tween{};
      damage();
    }
    if (m_drag.lifted) {
      const bool rmb = m_drag.button == BTN_RIGHT;
      m_lastDragBox = dragVisualBox();
      auto oldBoxes = captureCurrentBoxes(w);
      m_drag = {};
      int cardIdx = -1;
      for (int i = 0; i < static_cast<int>(m_strip.size()); ++i)
        if (m_strip[i].kind == model::StripItem::Kind::Ws &&
            stripCardAt(i).contains(lx, ly)) {
          cardIdx = i;
          break;
        }
      if (cardIdx >= 0) {
        debug::dbg("[REL] branch=Tile->stripCard(rmb=" + std::to_string(rmb) + ")");
        if (rmb) {
          PHLWINDOW vj;
          const auto &sit = m_strip[cardIdx];
          for (size_t j = 0; j < sit.wins.size(); ++j) {
            const auto v = sit.wins[j].win.lock();
            if (!v || v == w || !v->m_isMapped || v->isHidden())
              continue;
            if (stripWinSlotRect(sit, stripCardAt(cardIdx), j).contains(lx, ly)) {
              vj = v;
              break;
            }
          }
          if (vj) {
            const LRect vOld = stripWinSlotRect(sit,
                stripCardAt(cardIdx),
                [&]{ for (size_t j=0;j<sit.wins.size();++j) if(sit.wins[j].win.lock()==vj) return (int)j; return 0; }());
            if (swapWindows(w, vj)) {
              landAfterMove(w, m_lastDragBox, leaf("drag").ms, leaf("drag").curve);
              landAfterMove(vj, vOld, leaf("swap_partner").ms, leaf("swap_partner").curve);
              kickPulse(w); kickPulse(vj);
            } else damage();
          } else {
            swapOnWorkspace(w, m_strip[cardIdx]); // no slot hit → last-focused fallback
            kickPulse(w);
            landAfterMove(w, m_lastDragBox, leaf("drag").ms, leaf("drag").curve);
          }
        } else {
          dropOnStripCard(w, lx, ly, -1);
          kickPulse(w);
          landAfterMove(w, m_lastDragBox, leaf("drag").ms, leaf("drag").curve);
        }
        return true;
      }
      // Nearest tile within gap/2, not containment: a natural drop lands in the gap.
      if (!m_desktopMode && w &&
          cfg::behavior.drag_to_swap != 0) {
        int best = -1;
        double bestDist2 = 1e18;
        for (size_t i = 0; i < m_tiles.size(); ++i) {
          if (static_cast<int>(i) == press)
            continue;
          const LRect b = currentBox(m_tiles[i], static_cast<int>(i));
          const double cx = std::clamp(lx, b.x, b.x + b.w);
          const double cy = std::clamp(ly, b.y, b.y + b.h);
          const double dx = lx - cx, dy = ly - cy;
          const double d2 = dx * dx + dy * dy;
          if (d2 < bestDist2) {
            bestDist2 = d2;
            best = static_cast<int>(i);
          }
        }
        const double tol =
            std::max(0.0, cfg::grid.gap / 2.0);
        if (best >= 0 && bestDist2 <= tol * tol) {
          debug::dbg("[REL] branch=Tile->gridSwap");
          swapTiles(press, best);
          return true;
        }
      }
      // Desktop mode parks the preview; the box lives on the Tile so
      // rebuilds keep it.
      if (m_desktopMode && w) {
        debug::dbg("[REL] branch=Tile->desktopPark");
        const LRect cur =
            m_tiles[press].target;
        const LRect parked{lx - grabDX, ly - grabDY, cur.w, cur.h};
        auto &t = m_tiles[press];
        t.target = t.natural = parked;
        t.parked = true;
        m_hovered = -1;
        damage();
        return true;
      }
      debug::dbg("[REL] branch=Tile->flyback");
      replayReflow(oldBoxes);
      if (press < static_cast<int>(m_tiles.size())) {
        auto &t = m_tiles[press];
        t.natural = t.target;
        t.appear = 1.0;
      }
      beginSwapFX(w, m_lastDragBox, model::SwapStyle::Horizontal,
                  leaf("drag").ms, leaf("drag").curve);
      ensureAnimPump();
      damage();
      return true;
    }
    // close_trigger=doubleclick: defer the activate behind DBLCLICK_WINDOW, or
    // the overview is gone before the second click. Clear m_drag BEFORE
    // focusAndClose(): an armed press re-fires lifted=true mid-close.
    m_drag = {};
    if (closeOnDoubleClick() && w && g_pEventLoopManager) {
      if (m_clickTimer && m_pendingClickWin.lock() == w) {
        cancelPendingClick();
        closeTileWindow(press);
        return true;
      }
      cancelPendingClick(); // drop any OTHER tile's still-pending timer first
      m_pendingClickWin = w;
      m_clickTimer = makeShared<CEventLoopTimer>(
          DBLCLICK_WINDOW,
          [this](SP<CEventLoopTimer>, void *) {
            const auto pw = m_pendingClickWin.lock();
            m_pendingClickWin.reset();
            if (pw && m_active)
              focusAndClose(pw, Desktop::FOCUS_REASON_CLICK);
          },
          nullptr);
      g_pEventLoopManager->addTimer(m_clickTimer);
      return true;
    }
    focusAndClose(w, Desktop::FOCUS_REASON_CLICK);
    return true;
  }

  case model::Drag::Press::StripWin: {
    const int stripItem = m_drag.idx;
    const auto w = m_drag.win.lock();
    const bool rmb = m_drag.button == BTN_RIGHT;
    if (m_drag.lifted && m_drag.travelSq() < 64.0) {
      m_drag.lifted = false;
      m_dragLiftClock = anim::Tween{};
      damage();
    }
    const bool lifted = m_drag.lifted;
    const LRect fromBox = dragVisualBox();
    m_lastDragBox = fromBox;
    debug::dbg("[REL] press=StripWin lifted=" + std::to_string(lifted) +
               " rmb=" + std::to_string(rmb) +
               " dxMoved=" + std::to_string(m_drag.travelSq()) +
               " idx=" + std::to_string(m_drag.idx) +
               " winIdx=" + std::to_string(m_drag.winIdx) +
               " win=" + (w ? "valid" : "expired"));
    m_drag = {};
    if (lifted) {
      if (w) {
        // Slot hit swaps tiling slots for RMB or a same-workspace partner.
        for (size_t i = 0; i < m_strip.size(); ++i) {
          const auto &it = m_strip[i];
          if (it.kind != model::StripItem::Kind::Ws)
            continue;
          for (size_t j = 0; j < it.wins.size(); ++j) {
            const auto v = it.wins[j].win.lock();
            if (!v || v == w || !v->m_isMapped || v->isHidden())
              continue;
            if (!stripWinSlotRect(it, stripCardAt(i), j).contains(lx, ly))
              continue;
            const bool sameWs =
                v->m_workspace == w->m_workspace;
            if (!rmb && !sameWs)
              break;
            const LRect vOld = stripWinSlotRect(it, stripCardAt(i), j);
            if (swapWindows(w, v)) {
              debug::dbg("[REL] branch=StripWin->slotSwap");
              landAfterMove(w, fromBox, leaf("swap_main").ms,
                            leaf("swap_main").curve); // the initiator
              landAfterMove(v, vOld, leaf("swap_partner").ms,
                            leaf("swap_partner").curve); // the partner
              kickPulse(w);
              kickPulse(v);
              return true;
            }
            debug::dbg("[REL] branch=StripWin->slotSwapFailed");
            damage();
            return true;
          }
        }
      }
      if (w) {
        if (dropOnStripCard(w, lx, ly, stripItem)) {
          debug::dbg("[REL] branch=StripWin->dropOnStripCard");
          kickPulse(w);
          landAfterMove(w, fromBox, leaf("drag").ms, leaf("drag").curve);
          return true;
        }
        const int homeIdx = stripItemAt(lx, ly);
        if (homeIdx == stripItem || homeIdx < 0) {
          debug::dbg("[REL] branch=StripWin->snapBack");
          LRect ownSlot = fromBox;
          for (size_t si = 0; si < m_strip.size(); ++si) {
            const auto &sit = m_strip[si];
            if (sit.kind != model::StripItem::Kind::Ws)
              continue;
            for (size_t j = 0; j < sit.wins.size(); ++j) {
              if (sit.wins[j].win.lock() == w) {
                ownSlot = stripWinSlotRect(sit, stripCardAt(si), j);
                break;
              }
            }
          }
          beginSwapFX(w, fromBox,
                      parseSwapStyle(cfg::animStyle("strip_swap")),
                      leaf("drag").ms, leaf("drag").curve);
          // beginSwapFX may reuse a slot: force `from` on the entry it just pushed.
          if (!m_swapfx.empty())
            m_swapfx.back().from = fromBox;
          kickPulse(w);
          damage();
          return true;
        }
        // The strip band is NOT a drop target: releasing back over it is a cancel.
        bool onBand = false;
        if (const auto m = m_monitor.lock())
          onBand = stripBand().contains(lx, ly);
        const auto mm = m_monitor.lock();
        if (!onBand && mm &&
            LRect{0, 0, mm->m_size.x, mm->m_size.y}.contains(lx, ly)) {
          if (rmb) {
            int best = -1;
            double bestDist2 = 1e18;
            for (size_t i = 0; i < m_tiles.size(); ++i) {
              const auto tv = m_tiles[i].win.lock();
              if (!tv || tv == w) continue;
              const LRect b = currentBox(m_tiles[i], static_cast<int>(i));
              const double cx = std::clamp(lx, b.x, b.x + b.w);
              const double cy = std::clamp(ly, b.y, b.y + b.h);
              const double d2 = (lx-cx)*(lx-cx) + (ly-cy)*(ly-cy);
              if (d2 < bestDist2) { bestDist2 = d2; best = static_cast<int>(i); }
            }
            const double tol = std::max(0.0, cfg::grid.gap / 2.0 + 8.0);
            if (best >= 0 && bestDist2 <= tol * tol) {
              const auto vj = m_tiles[best].win.lock();
              const LRect vOld = currentBox(m_tiles[best], best);
              if (vj && swapWindows(w, vj)) {
                debug::dbg("[REL] branch=StripWin->gridTileSwap");
                landAfterMove(w,  fromBox, leaf("swap_main").ms,    leaf("swap_main").curve);
                landAfterMove(vj, vOld,   leaf("swap_partner").ms, leaf("swap_partner").curve);
                kickPulse(w); kickPulse(vj);
                return true;
              }
            }
            for (const auto &it : m_strip) {
              if (it.kind != model::StripItem::Kind::Plus &&
                  it.kind != model::StripItem::Kind::All && it.active) {
                swapOnWorkspace(w, it);
                debug::dbg("[REL] branch=StripWin->swapOnWorkspace(fallback)");
                kickPulse(w);
                landAfterMove(w, fromBox, leaf("drag").ms, leaf("drag").curve);
                return true;
              }
            }
          } else {
            for (const auto &it : m_strip) {
              if (it.kind != model::StripItem::Kind::Plus &&
                  it.kind != model::StripItem::Kind::All && it.active) {
                dropOnWorkspace(w, it);
                debug::dbg("[REL] branch=StripWin->dropOnWorkspace");
                kickPulse(w);
                landAfterMove(w, fromBox, leaf("drag").ms, leaf("drag").curve);
                return true;
              }
            }
          }
        }
      }
      if (lifted) {
        debug::dbg("[REL] branch=StripWin->flyback");
        if (auto *home = homeStripCardFor(w))
          switchToWorkspace(*home);
        beginSwapFX(w, fromBox, model::SwapStyle::Horizontal,
                    leaf("drag").ms, leaf("drag").curve);
      }
      damage();
      return true;
    }
    debug::dbg("[REL] branch=StripWin->shortClick");
    if (auto *home = homeStripCardFor(w))
      switchToWorkspace(*home);
    return true;
  }
  default:
    break;
  }

  if (cfg::behavior.exit_on_click != 0)
    close();
  return true;
}

// True if focusing `w` switches the monitor's real workspace (it slides).
bool Overview::crossesRealWorkspace(const PHLWINDOW &w) const {
  const auto m = m_monitor.lock();
  return w && m && w->m_workspace && w->m_workspace != m->m_activeWorkspace;
}

void Overview::focusAndClose(const PHLWINDOW &w, Desktop::eFocusReason reason) {
  // Cross-workspace commit must not focus here; m_pendingFocus defers to
  // deactivate(), after the overlay is gone.
  const bool deferFocus = crossesRealWorkspace(w);
  if (w) {
    if (const auto ws = w->m_workspace;
        ws && (showAllWorkspaces() || ws != m_workspace.lock())) {
      // Nothing is rebuilt or relaid out here, on purpose. focusAndClose runs
      // DURING the close, and layoutTiles() recomputes m_run from
      // showAllWorkspaces(): with m_allOverride already 0 the deck stopped being a
      // carousel BEFORE close() read it, so every card flew to its own window
      // box, all of which are the same fullscreen rectangle — the run played the
      // landing animation four times over and met in one point. The commit only
      // has to name a window; the exit keeps the layout it already has.
      m_allOverride = 0;
      m_workspace = ws;
      m_hovered = m_hoveredStrip = -1;
      m_selected = -1;
      for (size_t i = 0; i < m_tiles.size(); ++i)
        if (m_tiles[i].win.lock() == w) {
          m_selected = static_cast<int>(i);
          break;
        }
    }
  }
  m_pendingFocus = w;
  close();
  if (w && !deferFocus)
    Desktop::focusState()->fullWindowFocus(w, reason);
}

void Overview::cancelPendingClick() {
  m_pendingClickWin.reset();
  if (m_clickTimer) {
    m_clickTimer->cancel();
    if (g_pEventLoopManager)
      g_pEventLoopManager->removeTimer(m_clickTimer);
    m_clickTimer.reset();
  }
}

} // namespace gloview
