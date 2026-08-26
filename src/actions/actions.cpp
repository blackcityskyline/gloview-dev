#include "../config/config.hpp"
#include "../debug/log.hpp"
#include "../overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

bool Overview::dropOnStripCard(const PHLWINDOW &w, double lx, double ly,
                               int skipItem) {
  const int idx = stripItemAt(lx, ly);
  if (idx < 0 || idx == skipItem)
    return false;
  const auto &it = m_strip[idx];
  if (it.kind != model::StripItem::Kind::Ws) {
    // "+" / "All": the legacy whole-card semantics
    if (m_drag.button == BTN_RIGHT)
      swapOnWorkspace(w, it);
    else
      dropOnWorkspace(w, it);
    return true;
  }

  // Ws card: resolve the SLOT under the cursor — the drop is positional.
  // The half-zone the cursor is in names the NEIGHBOR window; dwindle splits
  // the last-focused window on the target workspace, so focusing the
  // neighbor right before the move gives a TRUE half-split (the moved
  // window takes one half of the neighbor, the neighbor keeps the other).
  PHLWINDOW vj;
  for (size_t j = 0; j < it.wins.size(); ++j) {
    const auto v = it.wins[j].win.lock();
    if (!v || v == w || !v->m_isMapped || v->isHidden())
      continue;
    if (stripWinSlotRect(it, stripCardAt(idx), j).contains(lx, ly)) {
      vj = v;
      break;
    }
  }

  // The drop takes the CHOSEN window's slot: a swap with it. (A true
  // half-split insert would need to focus the neighbor on the target
  // workspace before the move — CFocusState::rawWindowFocus ABORTS on
  // windows of non-active workspaces, so that path is unavailable until the
  // layout-insert support lands.)
  if (vj)
    swapWindows(w, vj); // precise slot swap with the chosen window
  else if (m_drag.button == BTN_RIGHT)
    swapOnWorkspace(w, it); // empty card / gap: last-focused fallback
  else
    dropOnWorkspace(w, it); // empty card / gap: plain move
}

void Overview::dropOnWorkspace(const PHLWINDOW &w, const model::StripItem &it) {
  if (w && w->m_workspace == it.ws.lock()) {
    damage(); // already there — a "move" would churn the whole layout
    return;
  }
  const auto m = m_monitor.lock();
  if (!w || !m) {
    damage();
    return;
  }

  PHLWORKSPACE target;
  if (it.kind == model::StripItem::Kind::Plus) {
    int id = 1;
    while (State::workspaceState()->query().id(id).run())
      ++id;
    target = State::workspaceState()->create(id, m->m_id);
    m_newCardId = id; // pop the new card in
    m_newCard.begin();
    m_newCardAnim = true;
  } else if (it.virtualWs) {
    // strip_empty_mode show/neighbors placeholder card (task #3): no real
    // workspace object exists yet at this specific id — create exactly that one
    // (unlike "+", which always takes the lowest free id) and hold it
    // persistent like any other freshly created workspace so it isn't reaped
    // before a window lands on it.
    target = State::workspaceState()->query().id(it.id).run();
    if (!target)
      target = State::workspaceState()->create(it.id, m->m_id);
    if (target) {
      target->setPersistent(true);
      m_newWorkspaces.push_back(target);
      m_newCardId = it.id;
      m_newCard.begin();
      m_newCardAnim = true;
    }
  } else
    target = it.ws.lock();

  if (!target || target == w->m_workspace) {
    damage(); // same workspace: nothing to do, tile snaps back
    return;
  }

  auto oldBoxes = captureCurrentBoxes(w);

  Desktop::globalWindowController()->moveWindowToWorkspace(w, target);

  // switch_on_drop: follow the window to its new workspace instead of staying
  // put.
  if (cfg::behavior.switch_on_drop != 0) {
    model::StripItem dst;
    dst.ws = target;
    switchToWorkspace(dst);
    return;
  }

  // Window left the displayed workspace; rebuild and glide the remaining tiles
  // into their new slots. replayReflow keeps the chrome settled (m_progress
  // pinned at 1 — no backdrop flash / strip re-slide); tiles render live,
  // nothing to recapture.
  replayReflow(oldBoxes);
  damage();
}

// Swap two previews' windows in the real Hyprland layout AND the overview.
// Mirrors dropOnWorkspace: mutate the real layout, then replayReflow() rebuilds
// tiles from the NEW geometry. Rebuild is load-bearing: a manual slot-swap
// desyncs the tile slot from the window's real goal(), so renderWindowLive maps
// the surface outside its tile and only the dark backing shows ("black"/empty
// preview).
//
// Cross-workspace pairs ARE swapped (task #2): ITarget::swap (which
// switchTargets calls) natively handles two targets on different spaces — it
// swaps their tree position AND their workspace membership, then recalculates
// both spaces — so dragging a tile from one workspace onto a tile from another
// in the all-workspaces (expo) grid works the same as a same-workspace swap.
// Only fullscreen windows (no well-defined tiled slot to trade) are excluded.
// Swap two REAL windows' tiling slots (same- or cross-workspace). Shared by
// the grid drag (swapTiles) and the strip RMB slot-to-slot drop. True if the
// swap happened. ITarget::swap() recalculates both spaces it touches, so no
// manual recalculate() is needed afterward.
bool Overview::swapWindows(const PHLWINDOW &wa, const PHLWINDOW &wb) {
  if (!wa || !wb || wa == wb ||
      Fullscreen::controller()->isFullscreen(wa) ||
      Fullscreen::controller()->isFullscreen(wb))
    return false;
  const auto ta = wa->layoutTarget();
  const auto tb = wb->layoutTarget();
  if (!g_layoutManager || !ta || !tb)
    return false;

  // capture where every tile sits NOW so the previews glide from their current
  // spots into the post-swap slots (same tail as dropOnWorkspace).
  auto oldBoxes = captureCurrentBoxes();
  g_layoutManager->switchTargets(ta, tb);
  // Rebuild the overview from the swapped real geometry and glide in.
  replayReflow(oldBoxes);
  kickPulse(wa);
  kickPulse(wb);
  damage();       // this frame's snapshot may already be taken — schedule
  ensureAnimPump(); // another promptly so the revealed area repaints at once
  return true;
}

void Overview::swapTiles(int a, int b) {
  if (a < 0 || b < 0 || a == b || a >= static_cast<int>(m_tiles.size()) ||
      b >= static_cast<int>(m_tiles.size())) {
    damage();
    return;
  }
  if (!swapWindows(m_tiles[a].win.lock(), m_tiles[b].win.lock()))
    damage();
}

// RMB-drop-on-a-strip-card counterpart to dropOnWorkspace (task #8): instead of
// MOVING `w` onto that workspace, SWAP it with the target workspace's
// last-focused window — both trade places (and tiling slot), each landing on
// the other's workspace. Falls back to a plain move when there's nothing on the
// target to swap with (empty workspace, or a fullscreen partner with no
// well-defined slot).
// Landing animations (anim leaf "drop"). beginSwapFX: the window now renders
// as a STRIP thumb (strip has no glide machinery of its own) — fly it from
// `from` into its slot. landAfterMove: dispatches per landing surface — strip
// -> flight, grid tile -> natural = oldBox so the existing tile glide flies it
// in (chrome moves with it).
model::SwapStyle Overview::stripSwapStyle() const {
  const auto s = cfg::anim.strip_swap_anim.get();
  if (s == "slidevert") return model::SwapStyle::SlideVert;
  if (s == "fade") return model::SwapStyle::Fade;
  if (s == "pop") return model::SwapStyle::Pop;
  return model::SwapStyle::Horizontal;
}

model::SwapStyle Overview::gridSwapStyle() const {
  const auto s = cfg::anim.grid_swap_anim.get();
  if (s == "slidevert") return model::SwapStyle::SlideVert;
  if (s == "fade") return model::SwapStyle::Fade;
  if (s == "pop") return model::SwapStyle::Pop;
  return model::SwapStyle::Horizontal;
}

void Overview::beginSwapFX(const PHLWINDOW &w, const LRect &from,
                           model::SwapStyle style) {
  if (!w || !w->m_isMapped || w->isHidden())
    return;
  std::erase_if(m_swapfx, [&w](const model::SwapFX &f) { return f.win.lock() == w; });
  model::SwapFX fx;
  fx.win   = w;
  fx.from  = from;
  fx.style = style;
  fx.clock.begin();
  m_swapfx.push_back(std::move(fx));
  ensureAnimPump();
  damage();
}

bool Overview::swapfxActive(const PHLWINDOW &w) const {
  for (const auto &l : m_swapfx)
    if (l.win.lock() == w)
      return true;
  return false;
}

void Overview::landAfterMove(const PHLWINDOW &w, const LRect &oldBox) {
  if (!w)
    return;
  bool strip = false;
  for (const auto &it : m_strip) {
    if (it.kind != model::StripItem::Kind::Ws)
      continue;
    for (const auto &sw : it.wins)
      if (sw.win.lock() == w)
        strip = true;
  }
  if (strip) { // strip thumbs have no glide machinery — always an FX flight
    beginSwapFX(w, oldBox, stripSwapStyle());
    return;
  }
  // grid tile: Horizontal keeps the natural->target tile glide (chrome flies
  // with it); the other styles replace the glide with an FX flight.
  if (gridSwapStyle() != model::SwapStyle::Horizontal) {
    beginSwapFX(w, oldBox, gridSwapStyle());
    return;
  }
  for (auto &t : m_tiles)
    if (t.win.lock() == w) {
      t.natural = oldBox; // the tile glide flies it in from the old box
      t.appear = 1.0;
      break;
    }
}

void Overview::swapOnWorkspace(const PHLWINDOW &w, const model::StripItem &it) {
  if (w && w->m_workspace == it.ws.lock()) {
    damage(); // nothing to swap with across workspaces
    return;
  }
  if (!w || it.kind == model::StripItem::Kind::Plus || it.kind == model::StripItem::Kind::All) {
    damage();
    return;
  }
  PHLWORKSPACE target = it.ws.lock();
  if (it.virtualWs && !target) {
    // nothing could possibly live on a workspace that doesn't exist yet — plain
    // move (which lazily creates it) is the only sensible outcome.
    dropOnWorkspace(w, it);
    return;
  }
  if (!target || target == w->m_workspace) {
    damage();
    return;
  }

  PHLWINDOW partner = target->m_lastFocusedWindow.lock();
  if (!partner || partner->m_workspace != target || !partner->m_isMapped ||
      partner->isHidden()) {
    partner.reset();
    for (const auto &cand : Desktop::windowState()->windows())
      if (cand && cand->m_isMapped && !cand->isHidden() &&
          cand->m_workspace == target) {
        partner = cand;
        break;
      }
  }
  if (!partner || Fullscreen::controller()->isFullscreen(w) ||
      Fullscreen::controller()->isFullscreen(partner)) {
    dropOnWorkspace(w, it); // nothing to swap with — fall back to a plain move
    return;
  }

  const auto ta = w->layoutTarget();
  const auto tb = partner->layoutTarget();
  if (!g_layoutManager || !ta || !tb) {
    damage();
    return;
  }

  auto oldBoxes = captureCurrentBoxes();

  g_layoutManager->switchTargets(ta, tb);

  if (cfg::behavior.switch_on_drop != 0) {
    model::StripItem dst;
    dst.ws = target;
    switchToWorkspace(dst);
    return;
  }

  replayReflow(oldBoxes);
  // landings: w flew from the drag preview (m_lastDragBox), the partner flew
  // from its old grid slot into w's old card (a strip thumb now).
  landAfterMove(w, m_lastDragBox);
  for (const auto &[win, box] : oldBoxes)
    if (win == partner)
      landAfterMove(partner, box);
  damage();
}

void Overview::switchToWorkspace(const model::StripItem &it) {
  const auto m = m_monitor.lock();
  if (!m)
    return;

  PHLWORKSPACE ws;
  if (it.kind == model::StripItem::Kind::Plus) {
    int id = 1;
    while (State::workspaceState()->query().id(id).run())
      ++id;
    ws = State::workspaceState()->create(id, m->m_id, "", false);
    if (!ws)
      return;
  } else if (it.virtualWs) {
    ws = State::workspaceState()->query().id(it.id).run();
    if (!ws)
      ws = State::workspaceState()->create(it.id, m->m_id, "", false);
    if (!ws)
      return;
    ws->setPersistent(
        true); // guard against instant reaping while empty, like "+"
    m_newWorkspaces.push_back(ws);
  } else if (const auto target = it.ws.lock()) {
    ws = target;
    if (ws == m_workspace.lock())
      return; // already showing it
  } else
    return;

  // Display the target inside the overview without changing the live desktop
  // yet; tiles render straight from each window's own live surface
  // (renderWindowLive), bypassing Hyprland's normal per-frame render path
  // entirely, so an inactive workspace's windows draw exactly like an active
  // one's — no real slide needed.
  m_workspace = ws;
  // The identity check inside renderBackdrop handles re-blurring when the
  // backdrop SOURCE changes (a featured fullscreen mpv window appearing or
  // disappearing).  Switching between two non-mpv workspaces shares the same
  // static wallpaper source — no re-blur needed, and re-blurring would
  // introduce unnecessary per-frame variation into the cached result.

  // Rebuild around the displayed workspace and keep the overview visually
  // settled; clicking strip cards should not replay the opening animation.
  m_hovered = m_hoveredStrip = -1;
  // The ws-switch slide: direction by the ws id order (higher id = from the
  // right). The old workspace's tiles become ghosts that slide OUT to the
  // opposite side; the newcomers slide IN from `dir`.
  const auto oldWs = m_workspace.lock();
  if (oldWs && ws && ws->m_id != oldWs->m_id)
    m_wsSlideDir = ws->m_id > oldWs->m_id ? 1 : -1;
  const auto oldBoxes = captureCurrentBoxes();
  buildTiles();
  buildStrip();
  layoutTiles();
  debug::dbg("SWITCH ->" + std::to_string(ws ? ws->m_id : -1) + " tiles=" +
      std::to_string(m_tiles.size()));
  // The frozen backdrop/blur belong to the PREVIOUS workspace's background.
  // The wallpaper TEXTURE identity does not change on a ws switch (same
  // noctalia layer object, repainted content), so the per-frame srcId check
  // in renderBackdrop() keeps the stale dark capture alive — previews then
  // snap old->new the instant their population settles and blur-behind
  // kicks in. Invalidate explicitly; the next frame re-captures.

  // Every new-workspace tile is a newcomer here: staggered slide/fade-in
  // replaces the old one-frame content swap (cards, digits, jump alike).
  // The old workspace's tiles slide out as ghosts instead of vanishing.
  for (auto &t : m_tiles)
    t.appear = 0.0;
  m_ghosts.clear();
  for (const auto &[oldWin, oldBox] : oldBoxes) {
    bool kept = false;
    for (const auto &t : m_tiles)
      if (t.win.lock() == oldWin) { kept = true; break; }
    if (!kept)
      m_ghosts.push_back(model::Ghost{oldWin, oldBox});
  }
  m_populate.begin();
  ensureAnimPump();
  m_tileClock.pinEnd(reflowDur());
  m_progress = 1.0;
  m_opening = true;
  m_timeline.pinEnd(animDuration()); // keep the overview visually settled
  damage();
}

void Overview::stepWorkspace(int dir) {
  if (m_strip.empty())
    return;
  // collect only the real workspace cards (skip the "+" and the optional "All"
  // card), then step within that list so the wrap can't land on a non-workspace
  // card.
  std::vector<int> real;
  int activePos = -1;
  for (size_t i = 0; i < m_strip.size(); ++i) {
    if (m_strip[i].kind == model::StripItem::Kind::Plus || m_strip[i].kind == model::StripItem::Kind::All)
      continue;
    if (m_strip[i].active)
      activePos = static_cast<int>(real.size());
    real.push_back(static_cast<int>(i));
  }
  if (real.empty())
    return; // only the "+"/"All" cards
  if (activePos < 0)
    activePos = 0;
  int next = activePos + (dir > 0 ? 1 : -1);
  if (next < 0)
    next = static_cast<int>(real.size()) - 1; // wrap to last
  else if (next >= static_cast<int>(real.size()))
    next = 0; // wrap to first
  if (next != activePos)
    switchToWorkspace(m_strip[real[next]]);
}

// "+" card: create a workspace, pop its card in, and (per
// switch_on_new_workspace) optionally follow the display to it.
// new_workspace_mode picks WHICH id: "fill" (default) takes the lowest free
// slot (so gaps left by closed workspaces get reused first); "linear" always
// appends past the highest existing id, never backfilling a gap.
void Overview::addWorkspace() {
  const auto m = m_monitor.lock();
  if (!m)
    return;
  int id = 1;
  if (cfg::behavior.new_workspace_mode.get() == "linear") {
    for (const auto &wref : State::workspaceState()->workspaces())
      if (const auto ws = wref.lock(); ws && !ws->m_isSpecialWorkspace)
        id = std::max(id, static_cast<int>(ws->m_id) + 1);
  } else {
    while (State::workspaceState()->query().id(id).run())
      ++id;
  }
  const auto ws = State::workspaceState()->create(id, m->m_id, "", false);
  if (!ws)
    return;
  // A new empty workspace is reaped within a frame or two unless focused. Hold
  // it persistent so its card survives while up; deactivate() releases it
  // (normal reaping applies after).
  ws->setPersistent(true);
  m_newWorkspaces.push_back(ws);
  m_newCardId = id;
  m_newCard.begin();
  m_newCardAnim = true;
  debug::dbg("added workspace " + std::to_string(id));
  if (cfg::behavior.switch_on_new_workspace != 0) {
    model::StripItem it;
    it.ws = ws;
    switchToWorkspace(
        it); // follow the display (rebuilds the strip with the new card)
  } else {
    m_hovered = m_hoveredStrip = -1;
    buildStrip(); // keep the current display; just surface the new card so it
                  // animates in
    damage();
  }
}

// Keyboard-only per-window close (key_close_window): send-close a single tile's
// window.
void Overview::closeTileWindow(int i) {
  if (i < 0 || i >= static_cast<int>(m_tiles.size()))
    return;
  const auto w = m_tiles[i].win.lock();
  if (!w)
    return;
  debug::dbg("close tile window");
  // sendClose is async — the client decides when to unmap. Don't touch m_tiles
  // here: syncTiles() (run each frame) notices the window vanish and reflows,
  // which also covers windows that close themselves while the overview is up.
  w->sendClose();
}

// Middle-click a workspace card: send-close every window on it (async, like the
// per-window middle-click). syncTiles() reflows once the windows actually go.
void Overview::closeWorkspaceWindows(const model::StripItem &it) {
  if (it.kind == model::StripItem::Kind::Plus || it.kind == model::StripItem::Kind::All)
    return;
  const auto ws = it.ws.lock();
  if (!ws)
    return;
  int n = 0;
  for (const auto &w : Desktop::windowState()->windows())
    if (w && w->m_isMapped && !w->isHidden() && w->m_workspace == ws) {
      w->sendClose();
      ++n;
    }
  debug::dbg("middle-click workspace: closed " + std::to_string(n) + " window(s)");
}

} // namespace gloview
