#include "overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

void Overview::dropOnWorkspace(const PHLWINDOW& w, const StripItem& it) {
    const auto m = m_monitor.lock();
    if (!w || !m) {
        damage();
        return;
    }

    PHLWORKSPACE target;
    if (it.isPlus) {
        int id = 1;
        while (g_pCompositor->getWorkspaceByID(id))
            ++id;
        target         = g_pCompositor->createNewWorkspace(id, m->m_id);
        m_newCardId    = id; // pop the new card in
        m_newCardStart = std::chrono::steady_clock::now();
        m_newCardAnim  = true;
    } else if (it.virtualWs) {
        // strip_empty_mode show/neighbors placeholder card (task #3): no real workspace
        // object exists yet at this specific id — create exactly that one (unlike "+", which
        // always takes the lowest free id) and hold it persistent like any other freshly
        // created workspace so it isn't reaped before a window lands on it.
        target = g_pCompositor->getWorkspaceByID(it.id);
        if (!target)
            target = g_pCompositor->createNewWorkspace(it.id, m->m_id);
        if (target) {
            target->setPersistent(true);
            m_newWorkspaces.push_back(target);
            m_newCardId    = it.id;
            m_newCardStart = std::chrono::steady_clock::now();
            m_newCardAnim  = true;
        }
    } else
        target = it.ws.lock();

    if (!target || target == w->m_workspace) {
        damage(); // same workspace: nothing to do, tile snaps back
        return;
    }

    std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
    oldBoxes.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        const auto win = m_tiles[i].win.lock();
        if (win && win != w)
            oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));
    }

    g_pCompositor->moveWindowToWorkspaceSafe(w, target);

    // switch_on_drop: follow the window to its new workspace instead of staying put.
    if (cfgInt("plugin:gloview:switch_on_drop", 0) != 0) {
        StripItem dst;
        dst.ws = target;
        switchToWorkspace(dst);
        return;
    }

    // Window left the displayed workspace; rebuild and glide the remaining tiles into
    // their new slots. replayReflow keeps the chrome settled (m_progress pinned at 1 —
    // no backdrop flash / strip re-slide); tiles render live, nothing to recapture.
    replayReflow(oldBoxes);
    damage();
}

// Swap two previews' windows in the real Hyprland layout AND the overview. Mirrors
// dropOnWorkspace: mutate the real layout, then replayReflow() rebuilds tiles from the
// NEW geometry. Rebuild is load-bearing: a manual slot-swap desyncs the tile slot from
// the window's real goal(), so renderWindowLive maps the surface outside its tile and
// only the dark backing shows ("black"/empty preview).
//
// Cross-workspace pairs ARE swapped (task #2): ITarget::swap (which switchTargets calls)
// natively handles two targets on different spaces — it swaps their tree position AND their
// workspace membership, then recalculates both spaces — so dragging a tile from one
// workspace onto a tile from another in the all-workspaces (expo) grid works the same as a
// same-workspace swap. Only fullscreen windows (no well-defined tiled slot to trade) are
// excluded.
void Overview::swapTiles(int a, int b) {
    if (a < 0 || b < 0 || a == b || a >= static_cast<int>(m_tiles.size()) || b >= static_cast<int>(m_tiles.size())) {
        damage();
        return;
    }
    const auto wa = m_tiles[a].win.lock();
    const auto wb = m_tiles[b].win.lock();
    if (!wa || !wb || wa == wb || wa->isFullscreen() || wb->isFullscreen()) {
        damage();
        return;
    }
    const auto ta = wa->layoutTarget();
    const auto tb = wb->layoutTarget();
    if (!g_layoutManager || !ta || !tb) {
        damage();
        return;
    }

    // capture where every tile sits NOW so the previews glide from their current spots
    // into the post-swap slots (same tail as dropOnWorkspace).
    std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
    oldBoxes.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i)
        if (const auto win = m_tiles[i].win.lock())
            oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));

    // Real swap. ITarget::swap() already recalculates both spaces it touches (same-space or
    // cross-space), so no manual recalculate() call is needed here afterward.
    g_layoutManager->switchTargets(ta, tb);

    // Rebuild the overview from the swapped real geometry and glide the tiles in. Tiles
    // render live, so there is nothing to recapture.
    replayReflow(oldBoxes);
    damage();
}

// RMB-drop-on-a-strip-card counterpart to dropOnWorkspace (task #8): instead of MOVING `w`
// onto that workspace, SWAP it with the target workspace's last-focused window — both trade
// places (and tiling slot), each landing on the other's workspace. Falls back to a plain move
// when there's nothing on the target to swap with (empty workspace, or a fullscreen partner
// with no well-defined slot).
void Overview::swapOnWorkspace(const PHLWINDOW& w, const StripItem& it) {
    if (!w || it.isPlus || it.isAll) {
        damage();
        return;
    }
    PHLWORKSPACE target = it.ws.lock();
    if (it.virtualWs && !target) {
        // nothing could possibly live on a workspace that doesn't exist yet — plain move
        // (which lazily creates it) is the only sensible outcome.
        dropOnWorkspace(w, it);
        return;
    }
    if (!target || target == w->m_workspace) {
        damage();
        return;
    }

    PHLWINDOW partner = target->m_lastFocusedWindow.lock();
    if (!partner || partner->m_workspace != target || !partner->m_isMapped || partner->isHidden()) {
        partner.reset();
        for (const auto& cand : g_pCompositor->m_windows)
            if (cand && cand->m_isMapped && !cand->isHidden() && cand->m_workspace == target) {
                partner = cand;
                break;
            }
    }
    if (!partner || w->isFullscreen() || partner->isFullscreen()) {
        dropOnWorkspace(w, it); // nothing to swap with — fall back to a plain move
        return;
    }

    const auto ta = w->layoutTarget();
    const auto tb = partner->layoutTarget();
    if (!g_layoutManager || !ta || !tb) {
        damage();
        return;
    }

    std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
    oldBoxes.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i)
        if (const auto win = m_tiles[i].win.lock())
            oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));

    g_layoutManager->switchTargets(ta, tb);

    if (cfgInt("plugin:gloview:switch_on_drop", 0) != 0) {
        StripItem dst;
        dst.ws = target;
        switchToWorkspace(dst);
        return;
    }

    replayReflow(oldBoxes);
    damage();
}

void Overview::switchToWorkspace(const StripItem& it) {
    const auto m = m_monitor.lock();
    if (!m)
        return;

    PHLWORKSPACE ws;
    if (it.isPlus) {
        int id = 1;
        while (g_pCompositor->getWorkspaceByID(id))
            ++id;
        ws = g_pCompositor->createNewWorkspace(id, m->m_id, "", false);
        if (!ws)
            return;
    } else if (it.virtualWs) {
        ws = g_pCompositor->getWorkspaceByID(it.id);
        if (!ws)
            ws = g_pCompositor->createNewWorkspace(it.id, m->m_id, "", false);
        if (!ws)
            return;
        ws->setPersistent(true); // guard against instant reaping while empty, like "+"
        m_newWorkspaces.push_back(ws);
    } else if (const auto target = it.ws.lock()) {
        ws = target;
        if (ws == m_workspace.lock())
            return; // already showing it
    } else
        return;

    // Display the target inside the overview without changing the live desktop yet;
    // captureSnapshots() force-renders inactive workspaces without a real slide.
    m_workspace = ws;

    // Rebuild around the displayed workspace and keep the overview visually
    // settled; clicking strip cards should not replay the opening animation.
    m_hovered = m_hoveredStrip = -1;
    buildTiles();
    buildStrip();
    layoutTiles();
    m_progress  = 1.0;
    m_opening   = true;
    m_reflowing = false;
    m_animStart = std::chrono::steady_clock::now() - std::chrono::milliseconds(std::max(1, cfgInt("plugin:gloview:duration", 360)));
    damage();
}

void Overview::stepWorkspace(int dir) {
    if (m_strip.empty())
        return;
    // collect only the real workspace cards (skip the "+" and the optional "All" card),
    // then step within that list so the wrap can't land on a non-workspace card.
    std::vector<int> real;
    int              activePos = -1;
    for (size_t i = 0; i < m_strip.size(); ++i) {
        if (m_strip[i].isPlus || m_strip[i].isAll)
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
        next = 0;                                 // wrap to first
    if (next != activePos)
        switchToWorkspace(m_strip[real[next]]);
}

// "+" card: create a workspace, pop its card in, and (per switch_on_new_workspace)
// optionally follow the display to it. new_workspace_mode picks WHICH id: "fill" (default)
// takes the lowest free slot (so gaps left by closed workspaces get reused first); "linear"
// always appends past the highest existing id, never backfilling a gap.
void Overview::addWorkspace() {
    const auto m = m_monitor.lock();
    if (!m)
        return;
    int id = 1;
    if (cfgStr("plugin:gloview:new_workspace_mode", "fill") == "linear") {
        for (const auto& wref : g_pCompositor->getWorkspaces())
            if (const auto ws = wref.lock(); ws && !ws->m_isSpecialWorkspace)
                id = std::max(id, static_cast<int>(ws->m_id) + 1);
    } else {
        while (g_pCompositor->getWorkspaceByID(id))
            ++id;
    }
    const auto ws = g_pCompositor->createNewWorkspace(id, m->m_id, "", false);
    if (!ws)
        return;
    // A new empty workspace is reaped within a frame or two unless focused. Hold it
    // persistent so its card survives while up; deactivate() releases it (normal reaping
    // applies after).
    ws->setPersistent(true);
    m_newWorkspaces.push_back(ws);
    m_newCardId    = id;
    m_newCardStart = std::chrono::steady_clock::now();
    m_newCardAnim  = true;
    dbg("added workspace " + std::to_string(id));
    if (cfgInt("plugin:gloview:switch_on_new_workspace", 1) != 0) {
        StripItem it;
        it.ws = ws;
        switchToWorkspace(it); // follow the display (rebuilds the strip with the new card)
    } else {
        m_hovered = m_hoveredStrip = -1;
        buildStrip(); // keep the current display; just surface the new card so it animates in
        damage();
    }
}

// Keyboard-only per-window close (key_close_window): send-close a single tile's window.
void Overview::closeTileWindow(int i) {
    if (i < 0 || i >= static_cast<int>(m_tiles.size()))
        return;
    const auto w = m_tiles[i].win.lock();
    if (!w)
        return;
    dbg("close tile window");
    // sendClose is async — the client decides when to unmap. Don't touch m_tiles
    // here: syncTiles() (run each frame) notices the window vanish and reflows,
    // which also covers windows that close themselves while the overview is up.
    w->sendClose();
}

// Middle-click a workspace card: send-close every window on it (async, like the
// per-window middle-click). syncTiles() reflows once the windows actually go.
void Overview::closeWorkspaceWindows(const StripItem& it) {
    if (it.isPlus || it.isAll)
        return;
    const auto ws = it.ws.lock();
    if (!ws)
        return;
    int n = 0;
    for (const auto& w : g_pCompositor->m_windows)
        if (w && w->m_isMapped && !w->isHidden() && w->m_workspace == ws) {
            w->sendClose();
            ++n;
        }
    dbg("middle-click workspace: closed " + std::to_string(n) + " window(s)");
}

} // namespace gloview
