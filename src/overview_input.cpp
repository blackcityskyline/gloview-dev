#include "overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

// ---- input ------------------------------------------------------------------

bool Overview::onMouseAxis(const IPointer::SAxisEvent &e) {
  if (!m_active)
    return false;
  // deltaDiscrete is in notches (±1); fall back to the continuous delta for
  // high-res/touchpad.
  const double notches = e.deltaDiscrete != 0
                             ? static_cast<double>(e.deltaDiscrete)
                             : e.delta / 15.0;

  // Wheel over the STRIP band scrolls the card group; over the MAIN area it
  // steps the displayed workspace (when enabled).
  bool overStrip = false;
  if (const auto m = m_monitor.lock()) {
    const auto mc = g_pInputManager->getMouseCoordsInternal();
    const LRect band = stripBand();
    const double lx = mc.x - m->m_position.x;
    const double ly = mc.y - m->m_position.y;
    overStrip = band.contains(lx, ly);
  }

  if (!overStrip &&
      cfgInt("plugin:gloview:scroll_switches_workspace", 1) != 0) {
    if (notches != 0.0)
      stepWorkspace(notches > 0 ? 1 : -1);
    return true;
  }

  // Modal: swallow the wheel even with nothing to scroll so it never reaches
  // windows behind.
  if (m_stripScrollMax <= 0.0)
    return true;
  const double step = stripThickness() * 0.9; // ~one card per notch
  m_stripScroll =
      std::clamp(m_stripScroll + notches * step, 0.0, m_stripScrollMax);
  updateHover(); // the card under the cursor changed
  damage();
  return true;
}

void Overview::onMouseMove() {
  // Remember where the cursor is so a move only repaints its old+new footprint
  // (renderCursorOnTop draws the SW cursor over our overlay). updateHover
  // already full-damages on hover *change* (its ring/highlight needs it), so
  // here we damage just the union of the cursor boxes instead of the whole
  // monitor on every move — on a Haswell iGPU a full-monitor damage per
  // mousemove was ~40% GPU with the overview up.
  const auto m = m_monitor.lock();
  if (!m || !Pointer::mgr()) {
    updateHover();
    return;
  }
  // If HW cursor is active there's nothing to erase/redraw — the KMS plane
  // handles it with zero framebuffer writes. Skip the union damage entirely.
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
  return (m_drag.press == Drag::Press::Tile && m_drag.lifted &&
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

  // drag tracking: promote an armed press (tile or strip-window slot) to a
  // real drag once the pointer passes a small threshold, then follow it.
  if (m_drag.armed()) {
    const double dx = lx - m_drag.pressX;
    const double dy = ly - m_drag.pressY;
    if (!m_drag.lifted && (dx * dx + dy * dy) > 64.0) // ~8px
      m_drag.lifted = true;
    if (m_drag.lifted) {
      m_drag.x = lx;
      m_drag.y = ly;
      m_hoveredStrip = stripItemAt(lx, ly); // card under the cursor, if any
      m_hovered = (m_drag.press == Drag::Press::Tile)
                      ? m_drag.idx
                      : -1; // -1 while dragging a strip window, which is correct
      damage();
      return;
    }
  }

  const int newTile = tileAt(lx, ly);
  const int newStrip = stripItemAt(lx, ly);
  if (newTile != m_hovered || newStrip != m_hoveredStrip) {
    m_hovered = newTile;
    m_hoveredStrip = newStrip;
    // keep the keyboard selection under the pointer so arrow-nav picks up where
    // the mouse left off (macOS-like). Only when actually over a tile.
    if (newTile >= 0 && cfgInt("plugin:gloview:focus_follows_mouse", 1) != 0) {
      m_selected = newTile;
      syncFocus(); // so a passthrough killactive/hotkey hits the hovered window
    }
    damage();
  }
}

namespace {
// close_trigger == "doubleclick": max gap between two clicks on the same tile
// to count as one double-click, instead of two independent single clicks. A
// touchpad "double-tap" arrives as the same two-click sequence, so this covers
// both without any separate handling.
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
    m_drag.press = Drag::Press::Empty;
    m_drag.button = e.button;

    // middle-click a workspace card → close every window on it (handled fully
    // on press). Per-window close is keyboard-only now (key_close_window, see
    // onKey).
    if (e.button == BTN_MIDDLE) {
      const int idx = stripItemAt(lx, ly);
      if (idx >= 0 && m_strip[idx].kind != StripItem::Kind::Plus && m_strip[idx].kind != StripItem::Kind::All)
        closeWorkspaceWindows(m_strip[idx]);
      return true; // swallow; middle is never a switch/drag/dismiss
    }

    // the "✕" button on a preview closes that window — shown in desktop mode,
    // or always when close_button_visibility is "always" (task 9); replaced
    // entirely by a tile double-click when close_trigger is "doubleclick"
    // (see the deferred single-click handling on release, below)
    if (!closeOnDoubleClick() && (m_desktopMode || closeButtonsAlwaysOn())) {
      for (size_t i = 0; i < m_tiles.size(); ++i) {
        const LRect lb =
            tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        const LRect br = closeButtonRect(lb);
        if (br.contains(lx, ly)) {
          m_drag.press =
              Drag::Press::Consumed; // the release must treat it as handled
          closeTileWindow(static_cast<int>(i));
          return true;
        }
      }
    }

    // strip card: the "✕" button (same visibility rule) closes every window on
    // it — the visible, discoverable counterpart to the middle-click shortcut
    // above (task 8).
    if (m_desktopMode || closeButtonsAlwaysOn()) {
      for (size_t i = 0; i < m_strip.size(); ++i) {
        if (m_strip[i].kind == StripItem::Kind::Plus || m_strip[i].kind == StripItem::Kind::All)
          continue;
        const LRect c = stripCardAt(i);
        const LRect br = closeButtonRect(c);
        if (br.contains(lx, ly)) {
          m_drag.press = Drag::Press::Consumed;
          closeWorkspaceWindows(m_strip[i]);
          return true;
        }
      }
    }

    // strip card: a press on a SPECIFIC window's preview slot arms a drag
    // candidate for that window (click vs. drag decided on release, mirroring a
    // main-grid tile) — lets you grab any window straight off the strip, not
    // just the currently displayed one. A press anywhere else on the card (or
    // on +/All) switches immediately, as before.
    for (size_t i = 0; i < m_strip.size(); ++i) {
      const LRect c = stripCardAt(i);
      if (!c.contains(lx, ly))
        continue;
      auto &it = m_strip[i];
      if (it.kind != StripItem::Kind::Plus && it.kind != StripItem::Kind::All) {
        for (size_t j = 0; j < it.wins.size(); ++j) {
          const auto w = it.wins[j].win.lock();
          if (!w || !w->m_isMapped || w->isHidden())
            continue;
          const LRect wb = stripWinSlotRect(it, c, j);
          if (wb.contains(lx, ly)) {
            m_drag.press   = Drag::Press::StripWin;
            m_drag.idx     = static_cast<int>(i);
            m_drag.winIdx  = static_cast<int>(j);
            m_drag.win     = w;
            m_drag.pressX = m_drag.x = lx;
            m_drag.pressY = m_drag.y = ly;
            m_drag.grabDX = lx - wb.x;
            m_drag.grabDY = ly - wb.y;
            return true;
          }
        }
      }
      m_drag.press = Drag::Press::StripCard;
      if (it.kind == StripItem::Kind::All)
        toggleAllWorkspaces();
      else if (it.kind == StripItem::Kind::Plus)
        addWorkspace();
      else
        switchToWorkspace(it);
      return true;
    }
    // window tile → arm a drag candidate; click vs drag decided on release
    if (const int hit = tileAt(lx, ly); hit >= 0) {
      m_drag.press = Drag::Press::Tile;
      m_drag.idx = hit;
      const LRect b = currentBox(m_tiles[hit], hit);
      m_drag.pressX = m_drag.x = lx;
      m_drag.pressY = m_drag.y = ly;
      m_drag.grabDX = lx - b.x;
      m_drag.grabDY = ly - b.y;
      return true;
    }
    // empty space
    m_drag.press = Drag::Press::Empty;
    return true;
  }

  // ---- release ----
  if (e.button == BTN_MIDDLE)
    return true; // middle was fully handled on press

  switch (m_drag.press) {
  case Drag::Press::StripCard:
  case Drag::Press::Consumed: { // switch / ✕ already handled on press
    m_drag = {};
    return true;
  }
  case Drag::Press::Tile: {
    const int press = m_drag.idx;
    const auto w = m_tiles[press].win.lock();
    const double grabDX = m_drag.grabDX, grabDY = m_drag.grabDY;
    if (m_drag.lifted) {
      m_drag = {};
      // dropped onto a workspace card → move the window there (RMB: swap
      // instead — task #8, mirrors the strip-window-drag drop branch below)
      if (dropOnStripCard(w, lx, ly, -1))
        return true;
      // grid mode: dropped onto (or near) another preview → swap the two
      // windows' places. Nearest-tile-within-tolerance rather than exact
      // containment: the natural drop point often lands in the gap between
      // tiles (especially with a wide `gap` config), and an exact-only hit test
      // there silently cancels the drag — which is what made swap feel
      // unreliable/"broken". (Skipped in desktop/canvas mode, where a drop
      // parks the preview instead.)
      if (!m_desktopMode && w &&
          cfgInt("plugin:gloview:drag_to_swap", 1) != 0) {
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
            std::max(0.0, cfgInt("plugin:gloview:gap", 34) / 2.0);
        if (best >= 0 && bestDist2 <= tol * tol) {
          swapTiles(press, best);
          return true;
        }
      }
      // desktop (canvas) mode: a drop just PARKS the preview where released.
      // Real window never floated/moved; the parked box lives on the Tile and
      // survives rebuilds via buildTiles()'s target carry.
      if (m_desktopMode && w) {
        const LRect cur =
            m_tiles[press].target; // keep the canvas size, move the corner
        const LRect parked{lx - grabDX, ly - grabDY, cur.w, cur.h};
        auto &t = m_tiles[press];
        t.target = t.natural = parked; // settled — currentBox returns it
        t.parked = true;
        m_hovered = -1;
        damage();
        return true;
      }
      damage(); // dropped in empty space → tile snaps back to its slot
      return true;
    }
    // A plain click activates that window — normally immediately (focus +
    // dismiss, see focusAndClose's expo-mode close-animation fix, task #5).
    // With close_trigger=doubleclick this can't fire immediately: the
    // overview would already be gone before a possible second click ever
    // arrives, so nothing could tell a genuine double-click apart from two
    // unrelated single clicks. Instead the activate is deferred behind
    // DBLCLICK_WINDOW; a second click on the SAME window before it fires
    // cancels the activate and closes the window instead (staying open) —
    // see cancelPendingClick() for how every other path that ends the
    // overview also drops this state so a stale timer can't fire later
    // against a tile that no longer means anything.
    if (closeOnDoubleClick() && w && g_pEventLoopManager) {
      if (m_clickTimer && m_pendingClickWin.lock() == w) {
        // second click on the same window inside the window → close it
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

  case Drag::Press::StripWin: {
    const int stripItem = m_drag.idx;
    const auto w = m_drag.win.lock();
    const bool rmb = m_drag.button == BTN_RIGHT;
    const bool lifted = m_drag.lifted;
    m_drag = {};
    if (lifted) {
      if (w && rmb) {
        // RMB onto a SPECIFIC window slot (any card, incl. the source's own
        // when it holds 2+ previews) → swap exactly these two windows'
        // tiling slots. Checked before the card-level fallbacks so an exact
        // slot hit always wins over "swap with card's last-focused".
        for (size_t i = 0; i < m_strip.size(); ++i) {
          const auto &it = m_strip[i];
          if (it.kind != StripItem::Kind::Ws)
            continue;
          for (size_t j = 0; j < it.wins.size(); ++j) {
            const auto v = it.wins[j].win.lock();
            if (!v || v == w || !v->m_isMapped || v->isHidden())
              continue;
            if (!stripWinSlotRect(it, stripCardAt(i), j).contains(lx, ly))
              continue;
            if (swapWindows(w, v))
              return true;
            damage(); // partner ineligible (fullscreen etc.) → snap back
            return true;
          }
        }
      }
      if (w) {
        // dropped onto a DIFFERENT card → move it there, same as a grid-tile
        // drop (RMB: swap with that workspace's window instead — task #8)
        if (dropOnStripCard(w, lx, ly, stripItem))
          return true;
        // dropped in the main preview area → send it to whichever workspace is
        // currently displayed there (equivalent to dropping it on that card).
        if (const auto m = m_monitor.lock();
            m && LRect{0, 0, m->m_size.x, m->m_size.y}.contains(lx, ly)) {
          for (const auto &it : m_strip) {
            if (it.kind != StripItem::Kind::Plus && it.kind != StripItem::Kind::All && it.active) {
              if (rmb)
                swapOnWorkspace(w, it);
              else
                dropOnWorkspace(w, it);
              return true;
            }
          }
        }
      }
      damage(); // dropped elsewhere (its own card, empty space) → snaps back
      return true;
    }
    // plain click on a strip window (no drag) → switch to its workspace, like
    // the card
    if (w) {
      for (auto &it : m_strip)
        if (it.kind != StripItem::Kind::Plus && it.kind != StripItem::Kind::All && it.ws.lock() == w->m_workspace) {
          switchToWorkspace(it);
          break;
        }
    }
    return true;
  }
  default:
    break;
  }

  if (cfgInt("plugin:gloview:exit_on_click", 1) != 0)
    close(); // released on empty space
  return true;
}

// Focus `w` and dismiss. Shared by the mouse-click path above and
// activateSelection() (Enter / Alt-Tab commit).
//
// Task #5: in the all-workspaces (expo) view, `w` can live on a workspace other
// than the one gloview had "displayed" (m_workspace). The naive old flow just
// reassigned m_workspace and called close() — since close() only STARTS the
// fade/glide, the closing animation kept showing the stale multi-workspace expo
// grid (every tile at its own on-screen spot, possibly on a workspace that's
// about to go invisible again) while the real desktop switched underneath
// essentially instantly (fullWindowFocus's cross-workspace path changes the
// active workspace synchronously) — so for the whole close animation the user
// watched the OLD expo grid fade out over the NEW (already-switched) desktop,
// then the mismatch resolved itself abruptly the instant the animation ended.
// Collapsing onto just the target workspace and rebuilding tiles BEFORE
// starting the close glide fixes this: the animation now targets each tile's
// real, final on-screen spot on the workspace that's about to actually be
// visible, so it reads as one continuous glide onto the clicked window instead
// of a cut.
//
// Bug fix (alt-tab/expo "two windows collide" glitch): the rebuild used to be
// gated ONLY on `ws != m_workspace.lock()` — i.e. only when the window we're
// landing on lives on a DIFFERENT workspace than whatever m_workspace happened
// to hold (the workspace that was active when the overview opened). That
// condition is false exactly when `w` sits on that original workspace — which
// includes the single most common alt-tab outcome: cycling around and landing
// back on the window that was already focused before the session started. In
// that case the rebuild was skipped entirely, so close() ran on the UNCOLLAPSED
// multi-workspace tile set: it reseeds EVERY tile's `natural` (not just `w`'s)
// to that window's own real position and glides ALL of them there
// simultaneously — including tiles for windows on OTHER, now-invisible
// workspaces, whose real ("natural") position reuses the very same monitor
// coordinates `w` is gliding to (most obviously when another workspace also
// holds a single, maximized window — its natural box is ~fullscreen too). The
// two glides visually collide into the same spot, and the loser simply vanishes
// once the real desktop is revealed underneath — the "two windows converge and
// one disappears" glitch. The fix: collapse out of expo any time we're actually
// IN expo
// (`showAllWorkspaces()`), not only when the target's workspace differs from
// `m_workspace` — that's the only thing that actually determines whether
// m_tiles still holds other-workspace tiles that need to be dropped before the
// close glide starts.
//
// Whether committing to `w` right now would make Hyprland switch the
// monitor's REAL active workspace synchronously. Verified against the pinned
// Hyprland source (FocusState.cpp CFocusState::rawWindowFocus): when the
// target's workspace isn't the currently VISIBLE one, fullWindowFocus() calls
// CMonitor::changeWorkspace(ws, /*internal=*/false, ...) right there on the
// spot — and `internal=false` is exactly what makes changeWorkspace() kick off
// Hyprland's own native workspace-switch slide via
// g_pDesktopAnimationManager->startAnimation(...) (Monitor.cpp). Called from
// two places: focusAndClose (to decide whether to defer the real focus call)
// and nowhere else — kept as a member so both agree on the same definition.
bool Overview::crossesRealWorkspace(const PHLWINDOW &w) const {
  const auto m = m_monitor.lock();
  return w && m && w->m_workspace && w->m_workspace != m->m_activeWorkspace;
}

void Overview::focusAndClose(const PHLWINDOW &w, Desktop::eFocusReason reason) {
  // Bug: Hyprland's native workspace-switch animation played visibly BEFORE
  // gloview's own close glide (a jarring double-transition) whenever an
  // Alt-Tab/expo commit landed on a window living on a workspace other than
  // the monitor's real, currently-visible one. Root cause: the unconditional
  // `Desktop::focusState()->fullWindowFocus(w, reason)` at the bottom of this
  // function used to fire IMMEDIATELY, i.e. at the START of the close glide —
  // and for a cross-workspace target, fullWindowFocus's own cross-workspace
  // path calls CMonitor::changeWorkspace(..., /*internal=*/false, ...)
  // synchronously right then, which is precisely what starts Hyprland's native
  // slide (see crossesRealWorkspace()'s comment). The fix: for a cross-
  // workspace commit, skip that immediate call entirely and let it happen
  // through the EXISTING m_pendingFocus mechanism instead — deactivate()
  // already re-does both the real changeWorkspace() and the fullWindowFocus()
  // together, but only once gloview's own close animation has FULLY finished
  // (m_pendingDeactivate, gated on progress reaching 0/1 — see its comment in
  // overview.hpp and updateAnimation()). That means Hyprland's native
  // animation, if any, only ever starts after our own overlay is completely
  // gone — never underneath or before it. Same-workspace commits don't touch
  // the active workspace at all, so focusing them immediately is still safe
  // and keeps focus (and any passthrough keybinds) in lockstep during the
  // glide, exactly as before.
  const bool deferFocus = crossesRealWorkspace(w);
  if (w) {
    if (const auto ws = w->m_workspace;
        ws && (showAllWorkspaces() || ws != m_workspace.lock())) {
      // Capture every currently-shown tile's ON-SCREEN box (its slot in
      // whatever's visible right now — e.g. the multi-workspace expo grid)
      // BEFORE collapsing m_tiles down to just `ws`'s own windows below.
      // Without this, buildTiles()+layoutTiles() hands every surviving tile
      // a BRAND NEW grid slot computed from scratch for the collapsed
      // single-workspace layout, and close() (called right after) treats
      // THAT as the glide's starting point — so the tile teleports there in
      // one frame before the close/zoom glide even begins: a hard cut
      // sandwiched between the alt-tab navigation and the close animation.
      // Reusing the captured box as the rebuilt tile's `target` instead
      // makes close()'s glide (target -> real window position, see its own
      // comment) start from exactly where the tile visually already was, so
      // the workspace collapse and the close/focus zoom read as one
      // continuous motion instead of a jump followed by an animation.
      std::unordered_map<void *, LRect> oldBoxes;
      oldBoxes.reserve(m_tiles.size());
      for (size_t i = 0; i < m_tiles.size(); ++i)
        if (const auto win = m_tiles[i].win.lock())
          oldBoxes.emplace(win.get(), currentBox(m_tiles[i], static_cast<int>(i)));

      m_allOverride = 0; // leave expo — showAllWorkspaces() must reflect
                         // single-ws before rebuild
      m_workspace = ws;
      m_hovered = m_hoveredStrip = -1;
      buildTiles();
      buildStrip();
      layoutTiles();
      for (auto &t : m_tiles) {
        if (const auto win = t.win.lock()) {
          const auto it = oldBoxes.find(win.get());
          if (it != oldBoxes.end())
            t.target = it->second;
        }
      }
      m_selected = -1;
      for (size_t i = 0; i < m_tiles.size(); ++i)
        if (m_tiles[i].win.lock() == w) {
          m_selected = static_cast<int>(i);
          break;
        }
    }
  }
  // Re-asserted once more at the very end of deactivate(), after the real
  // workspace switch settles — see m_pendingFocus's comment in overview.hpp.
  // For a cross-workspace commit (deferFocus) that reassertion is now the
  // ONLY real focus call — see this function's own comment above.
  m_pendingFocus = w;
  close();
  if (w && !deferFocus)
    Desktop::focusState()->fullWindowFocus(w, reason);
}

// Drops any pending close_trigger=doubleclick single-click timer (see its
// state's comment in overview.hpp). Called before arming a new one (a click on
// a DIFFERENT tile shouldn't leave the previous one's activate lying around to
// fire later out of nowhere) and by every path that can end the session (close
// / hardClose / dtor) or start a fresh one (open), so a stale timer can never
// fire against a tile from a session that's already gone.
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
