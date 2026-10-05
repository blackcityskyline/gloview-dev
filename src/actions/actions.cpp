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
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

model::StripItem *Overview::homeStripCardFor(const PHLWINDOW &w) {
  if (!w)
    return nullptr;
  for (auto &it : m_strip)
    if (it.kind == model::StripItem::Kind::Ws && it.ws.lock() == w->m_workspace)
      return &it;
  return nullptr;
}

bool Overview::dropOnStripCard(const PHLWINDOW &w, double lx, double ly,
                               int skipItem) {
  const int idx = stripItemAt(lx, ly);
  if (idx < 0 || idx == skipItem)
    return false;
  // COPY: the rebuild below frees the element a reference would dangle on.
  const auto it = m_strip[idx];
  if (it.kind != model::StripItem::Kind::Ws) {
    if (m_drag.button == BTN_RIGHT)
      swapOnWorkspace(w, it);
    else
      dropOnWorkspace(w, it);
    return true;
  }

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

  // assignToSpace splits beside vj without focusing it (RASSERT off-ws).
  if (vj && cfg::behavior.tile_layout.get() == "dwindle") {
    const auto tvj = vj->layoutTarget();
    const auto tw  = w->layoutTarget();
    if (tw && tvj && tvj->space()) {
      auto oldBoxes = captureCurrentBoxes(w);
      const auto vjPos  = vj->positionAnimation()->goal();
      const auto vjSize = vj->sizeAnimation()->goal();
      const Vector2D focalPoint{vjPos.x + vjSize.x / 2.0,
                                vjPos.y + vjSize.y / 2.0};
      tw->assignToSpace(tvj->space(), focalPoint);
      replayReflow(oldBoxes);
      damage();
      ensureAnimPump();
      return true;
    }
  }

  if (vj)
    swapWindows(w, vj);
  else if (m_drag.button == BTN_RIGHT)
    swapOnWorkspace(w, it);
  else
    dropOnWorkspace(w, it);
  return true;
}

void Overview::dropOnWorkspace(const PHLWINDOW &w, const model::StripItem &it) {
  if (w && w->m_workspace == it.ws.lock()) {
    damage();
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
    m_newCardId = id;
    m_clocks.newCard.begin();
    m_newCardAnim = true;
  } else if (it.virtualWs) {
    // Placeholder card: create exactly this id, persistent or it is reaped.
    target = State::workspaceState()->query().id(it.id).run();
    if (!target)
      target = State::workspaceState()->create(it.id, m->m_id);
    if (target) {
      target->setPersistent(true);
      m_newWorkspaces.push_back(target);
      m_newCardId = it.id;
      m_clocks.newCard.begin();
      m_newCardAnim = true;
    }
  } else
    target = it.ws.lock();

  if (!target || target == w->m_workspace) {
    damage();
    return;
  }

  auto oldBoxes = captureCurrentBoxes(w);

  Desktop::globalWindowController()->moveWindowToWorkspace(w, target);

  if (cfg::behavior.switch_on_drop != 0) {
    model::StripItem dst;
    dst.ws = target;
    switchToWorkspace(dst);
    return;
  }

  // replayReflow keeps the chrome settled (m_progress pinned at 1).
  replayReflow(oldBoxes);
  damage();
}

// Rebuild is load-bearing: without it the tile slot desyncs from the
// window's real goal().
bool Overview::swapWindows(const PHLWINDOW &wa, const PHLWINDOW &wb) {
  if (!wa || !wb || wa == wb ||
      Fullscreen::controller()->isFullscreen(wa) ||
      Fullscreen::controller()->isFullscreen(wb))
    return false;
  const auto ta = wa->layoutTarget();
  const auto tb = wb->layoutTarget();
  if (!g_layoutManager || !ta || !tb)
    return false;

  auto oldBoxes = captureCurrentBoxes();
  g_layoutManager->switchTargets(ta, tb);
  replayReflow(oldBoxes);
  for (const auto &[win, box] : oldBoxes)
    if (win == wa)
      landAfterMove(win, box, leaf("swap_main").ms,
                    leaf("swap_main").curve); // the initiator
    else if (win == wb)
      landAfterMove(win, box, leaf("swap_partner").ms,
                    leaf("swap_partner").curve); // the partner
  kickPulse(wa);
  kickPulse(wb);
  damage();
  ensureAnimPump();
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

// RMB on a strip card swaps `w` with that workspace's last-focused window.

void Overview::beginSwapFX(const PHLWINDOW &w, const LRect &from,
                           model::SwapStyle style, double ms,
                           const std::string &curve) {
  if (!w || !w->m_isMapped || w->isHidden())
    return;
  std::erase_if(m_swapfx, [&w](const model::SwapFX &f) { return f.win.lock() == w; });
  model::SwapFX fx;
  fx.win   = w;
  fx.from  = from;
  fx.style = style;
  fx.ms    = ms;
  fx.curve = curve;
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

void Overview::landAfterMove(const PHLWINDOW &w, const LRect &oldBox,
                             double ms, const std::string &curve) {
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
    beginSwapFX(w, oldBox, parseSwapStyle(cfg::animStyle("strip_swap")), ms, curve);
    return;
  }
  if (parseSwapStyle(cfg::animStyle("grid_swap")) !=
    model::SwapStyle::Horizontal) {
    beginSwapFX(w, oldBox, parseSwapStyle(cfg::animStyle("grid_swap")),
                ms, curve);
    return;
  }
  for (auto &t : m_tiles)
    if (t.win.lock() == w) {
      t.natural = oldBox;
      t.appear = 1.0;
      break;
    }
}

void Overview::swapOnWorkspace(const PHLWINDOW &w, const model::StripItem &it) {
  if (w && w->m_workspace == it.ws.lock()) {
    damage();
    return;
  }
  if (!w || it.kind == model::StripItem::Kind::Plus || it.kind == model::StripItem::Kind::All) {
    damage();
    return;
  }
  PHLWORKSPACE target = it.ws.lock();
  if (it.virtualWs && !target) {
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
    dropOnWorkspace(w, it);
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
  landAfterMove(w, m_lastDragBox, leaf("drag").ms, leaf("drag").curve);
  for (const auto &[win, box] : oldBoxes)
    if (win == partner)
      landAfterMove(partner, box, leaf("swap_partner").ms,
                    leaf("swap_partner").curve);
  damage();
}

void Overview::switchToWorkspace(const model::StripItem &it, bool instant) {
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
        true);
    m_newWorkspaces.push_back(ws);
  } else if (const auto target = it.ws.lock()) {
    ws = target;
    if (ws == m_workspace.lock())
    return;
  } else
    return;

  // Read the PREVIOUS ws BEFORE reassigning: m_wsSlideDir arms from the id order.
  const auto oldWs = m_workspace.lock();
  m_workspace = ws;

  m_hovered = m_hoveredStrip = -1;
  if (oldWs && ws && ws->m_id != oldWs->m_id)
    m_wsSlideDir = ws->m_id > oldWs->m_id ? 1 : -1;
  // The frozen backdrop belongs to the PREVIOUS ws — do NOT invalidate it.
  const auto oldBoxes = captureCurrentBoxes();
  buildTiles();
  buildStrip();
  layoutTiles();
  debug::dbg("SWITCH ->" + std::to_string(ws ? ws->m_id : -1) + " tiles=" +
      std::to_string(m_tiles.size()));
  // instant=true (jump mode) skips the ws-slide so close() cannot collide.
  if (instant) {
    m_wsSlideDir = 0;
    m_jumpMode   = true;
    m_ghosts.clear();
    for (auto &t : m_tiles)
      t.appear = 1.0;
    m_clocks.rebuild.pinEnd(std::max(m_entry.ms, m_ghost.ms));
    m_clocks.tiles.pinEnd(glideDur());
    m_progress = 1.0;
    m_opening = true;
    m_clocks.master.pinEnd(animDuration());
    ensureAnimPump();
    damage();
    return;
  }
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
  m_clocks.rebuild.begin();
  ensureAnimPump();
  m_clocks.tiles.pinEnd(glideDur());
  m_progress = 1.0;
  m_opening = true;
  m_clocks.master.pinEnd(animDuration());
  damage();
}

void Overview::stepWorkspace(int dir) {
  if (m_strip.empty())
    return;
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
    return;
  if (activePos < 0)
    activePos = 0;
  int next = activePos + (dir > 0 ? 1 : -1);
  if (next < 0)
    next = static_cast<int>(real.size()) - 1;
  else if (next >= static_cast<int>(real.size()))
    next = 0;
  if (next != activePos)
    switchToWorkspace(m_strip[real[next]]);
}

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
  // Persistent, or a new empty workspace is reaped within a frame.
  ws->setPersistent(true);
  m_newWorkspaces.push_back(ws);
  m_newCardId = id;
  m_clocks.newCard.begin();
  m_newCardAnim = true;
  debug::dbg("added workspace " + std::to_string(id));
  if (cfg::behavior.switch_on_new_workspace != 0) {
    model::StripItem it;
    it.ws = ws;
    switchToWorkspace(
        it);
  } else {
    m_hovered = m_hoveredStrip = -1;
    buildStrip();
    damage();
  }
}

void Overview::closeTileWindow(int i) {
  if (i < 0 || i >= static_cast<int>(m_tiles.size()))
    return;
  const auto w = m_tiles[i].win.lock();
  if (!w)
    return;
  debug::dbg("close tile window");
  // sendClose is async; syncTiles() reflows once the window vanishes.
  w->sendClose();
}

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
