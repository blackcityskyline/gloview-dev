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
#include <hyprland/src/managers/PointerManager.hpp>
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
    overStrip = (lx >= band.x && ly >= band.y && lx <= band.x + band.w &&
                 ly <= band.y + band.h);
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
  updateHover();
  // We repaint the cursor ourselves (renderCursorOnTop). updateHover only
  // damages on hover *change*, so moving within one tile would leave the old
  // cursor → trail. Damage every move.
  if (m_active)
    damage();
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

  // drag tracking: promote a pressed tile (or a pressed strip window) to a real
  // drag once the pointer passes a small threshold, then follow the cursor.
  if (m_pressTile >= 0 || m_pressStripItem >= 0) {
    const double dx = lx - m_pressX;
    const double dy = ly - m_pressY;
    if (!m_dragging && (dx * dx + dy * dy) > 64.0) // ~8px
      m_dragging = true;
    if (m_dragging) {
      m_dragX = lx;
      m_dragY = ly;
      int newStrip = -1; // highlight the workspace card under the cursor
      for (size_t i = 0; i < m_strip.size(); ++i) {
        const LRect c = stripCardAt(i);
        if (lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h)
          newStrip = static_cast<int>(i);
      }
      m_hoveredStrip = newStrip;
      m_hovered =
          m_pressTile; // -1 while dragging a strip window, which is correct
      damage();
      return;
    }
  }

  int newTile = -1;
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    const LRect b = currentBox(m_tiles[i], static_cast<int>(i));
    if (lx >= b.x && ly >= b.y && lx <= b.x + b.w && ly <= b.y + b.h)
      newTile = static_cast<int>(i);
  }
  int newStrip = -1;
  for (size_t i = 0; i < m_strip.size(); ++i) {
    const LRect c = stripCardAt(i);
    if (lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h)
      newStrip = static_cast<int>(i);
  }
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
constexpr int PRESS_NONE = -1;
constexpr int PRESS_STRIP =
    -2; // press landed on a strip card (switch happened)
constexpr int PRESS_EMPTY =
    -3; // press landed on empty space (close on release)
constexpr int PRESS_CONSUMED =
    -4; // press fully handled (e.g. desktop ✕) — release must do nothing
// BTN_MIDDLE (0x112) comes from linux/input-event-codes.h, pulled in
// transitively.
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
    m_pressTile = PRESS_NONE;
    m_dragging = false;
    m_pressButton = e.button;
    m_pressStripItem = -1;
    m_pressStripWin = -1;
    m_dragStripWin.reset();

    // middle-click a workspace card → close every window on it (handled fully
    // on press). Per-window close is keyboard-only now (key_close_window, see
    // onKey).
    if (e.button == BTN_MIDDLE) {
      for (size_t i = 0; i < m_strip.size(); ++i) {
        const LRect c = stripCardAt(i);
        if (!m_strip[i].isPlus && !m_strip[i].isAll && lx >= c.x && ly >= c.y &&
            lx <= c.x + c.w && ly <= c.y + c.h) {
          closeWorkspaceWindows(m_strip[i]);
          break;
        }
      }
      return true; // swallow; middle is never a switch/drag/dismiss
    }

    // the "✕" button on a preview closes that window — shown in desktop mode,
    // or always when close_button_visibility is "always" (task 9)
    if (m_desktopMode || closeButtonsAlwaysOn()) {
      for (size_t i = 0; i < m_tiles.size(); ++i) {
        const LRect lb =
            tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        const LRect br = closeButtonRect(lb);
        if (lx >= br.x && ly >= br.y && lx <= br.x + br.w &&
            ly <= br.y + br.h) {
          m_pressTile = PRESS_CONSUMED; // so the release doesn't treat it as an
                                        // empty-space click
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
        if (m_strip[i].isPlus || m_strip[i].isAll)
          continue;
        const LRect c = stripCardAt(i);
        const LRect br = stripCloseButtonRect(c);
        if (lx >= br.x && ly >= br.y && lx <= br.x + br.w &&
            ly <= br.y + br.h) {
          m_pressTile = PRESS_CONSUMED;
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
      if (!(lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h))
        continue;
      auto &it = m_strip[i];
      if (!it.isPlus && !it.isAll) {
        for (size_t j = 0; j < it.wins.size(); ++j) {
          const auto w = it.wins[j].win.lock();
          if (!w || !w->m_isMapped || w->isHidden())
            continue;
          const LRect wb = stripWinSlotRect(it, c, j);
          if (lx >= wb.x && ly >= wb.y && lx <= wb.x + wb.w &&
              ly <= wb.y + wb.h) {
            m_pressStripItem = static_cast<int>(i);
            m_pressStripWin = static_cast<int>(j);
            m_dragStripWin = w;
            m_pressX = m_dragX = lx;
            m_pressY = m_dragY = ly;
            m_grabDX = lx - wb.x;
            m_grabDY = ly - wb.y;
            return true;
          }
        }
      }
      m_pressTile = PRESS_STRIP;
      if (it.isAll)
        toggleAllWorkspaces();
      else if (it.isPlus)
        addWorkspace();
      else
        switchToWorkspace(it);
      return true;
    }
    // window tile → arm a drag candidate; click vs drag decided on release
    for (size_t i = 0; i < m_tiles.size(); ++i) {
      const LRect b = currentBox(m_tiles[i], static_cast<int>(i));
      if (lx >= b.x && ly >= b.y && lx <= b.x + b.w && ly <= b.y + b.h) {
        m_pressTile = static_cast<int>(i);
        m_pressX = m_dragX = lx;
        m_pressY = m_dragY = ly;
        m_grabDX = lx - b.x;
        m_grabDY = ly - b.y;
        return true;
      }
    }
    // empty space
    m_pressTile = PRESS_EMPTY;
    return true;
  }

  // ---- release ----
  if (e.button == BTN_MIDDLE) {
    m_pressTile = PRESS_NONE;
    return true; // middle was fully handled on press
  }
  const int press = m_pressTile;
  m_pressTile = PRESS_NONE;

  if (press == PRESS_STRIP || press == PRESS_CONSUMED)
    return true; // switch / ✕ already handled on press; ignore the release

  if (press >= 0 && press < static_cast<int>(m_tiles.size())) {
    const auto w = m_tiles[press].win.lock();
    if (m_dragging) {
      m_dragging = false;
      // dropped onto a workspace card → move the window there (RMB: swap
      // instead — task #8, mirrors the strip-window-drag drop branch below)
      for (size_t i = 0; i < m_strip.size(); ++i) {
        const LRect c = stripCardAt(i);
        if (lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h) {
          if (m_pressButton == BTN_RIGHT)
            swapOnWorkspace(w, m_strip[i]);
          else
            dropOnWorkspace(w, m_strip[i]);
          return true;
        }
      }
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
      // Real window never floated/moved; m_canvasPos survives per-frame
      // rebuilds.
      if (m_desktopMode && w) {
        const LRect cur =
            m_tiles[press].target; // keep the canvas size, move the corner
        const LRect parked{lx - m_grabDX, ly - m_grabDY, cur.w, cur.h};
        m_canvasPos[w.get()] = parked;
        m_tiles[press].target = parked;
        m_tiles[press].natural =
            parked; // settled — currentBox returns it directly
        m_hovered = -1;
        damage();
        return true;
      }
      damage(); // dropped in empty space → tile snaps back to its slot
      return true;
    }
    // a plain click → focus that window and dismiss (see focusAndClose for the
    // expo-mode close-animation fix, task #5).
    focusAndClose(w, Desktop::FOCUS_REASON_CLICK);
    return true;
  }

  // strip-card window drag: release decides click-to-switch vs. drop-to-move
  if (m_pressStripItem >= 0) {
    const int stripItem = m_pressStripItem;
    const auto w = m_dragStripWin.lock();
    m_pressStripItem = -1;
    m_pressStripWin = -1;
    m_dragStripWin.reset();

    if (m_dragging) {
      m_dragging = false;
      if (w) {
        // dropped onto a DIFFERENT card → move it there, same as a grid-tile
        // drop (RMB: swap with that workspace's window instead — task #8)
        for (size_t i = 0; i < m_strip.size(); ++i) {
          if (static_cast<int>(i) == stripItem)
            continue;
          const LRect c = stripCardAt(i);
          if (lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h) {
            if (m_pressButton == BTN_RIGHT)
              swapOnWorkspace(w, m_strip[i]);
            else
              dropOnWorkspace(w, m_strip[i]);
            return true;
          }
        }
        // dropped in the main preview area → send it to whichever workspace is
        // currently displayed there (equivalent to dropping it on that card).
        if (const auto m = m_monitor.lock();
            m && lx >= 0 && ly >= 0 && lx <= m->m_size.x && ly <= m->m_size.y) {
          for (const auto &it : m_strip) {
            if (!it.isPlus && !it.isAll && it.active) {
              if (m_pressButton == BTN_RIGHT)
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
        if (!it.isPlus && !it.isAll && it.ws.lock() == w->m_workspace) {
          switchToWorkspace(it);
          break;
        }
    }
    return true;
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
void Overview::focusAndClose(const PHLWINDOW &w, Desktop::eFocusReason reason) {
  if (w) {
    if (const auto ws = w->m_workspace;
        ws && (showAllWorkspaces() || ws != m_workspace.lock())) {
      m_allOverride = 0; // leave expo — showAllWorkspaces() must reflect
                         // single-ws before rebuild
      m_workspace = ws;
      m_hovered = m_hoveredStrip = -1;
      buildTiles();
      buildStrip();
      layoutTiles();
      m_selected = -1;
      for (size_t i = 0; i < m_tiles.size(); ++i)
        if (m_tiles[i].win.lock() == w) {
          m_selected = static_cast<int>(i);
          break;
        }
    }
  }
  close();
  if (w)
    Desktop::focusState()->fullWindowFocus(w, reason);
}

} // namespace gloview
