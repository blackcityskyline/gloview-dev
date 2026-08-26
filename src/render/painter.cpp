#include <algorithm>
#include <chrono>
#include <cmath>

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include "gl_util.hpp"
#include "../config/config.hpp"
#include "../debug/log.hpp"
#include "../overview.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

// The ONE pass element the plugin adds to Hyprland's render pass. Its draw()
// is the painter: a single call stack that authors the whole overlay
// top-to-bottom (see Overview::paint for the z-slots). Window content goes
// through the immediate leaf (window_content.cpp) at its z-slot, so chrome
// and content share one order — the queue-era phase machinery (six COverlayPass
// phases interleaved with queued CSurfacePassElements) is gone, and with it
// the entire class of queue-order bugs (ghosted siblings via currentFB
// sampling, occlusion on wrong footprints, phase-ordering constraints).
class PainterPass final : public IPassElement {
public:
  explicit PainterPass(Overview *o) : m_owner(o) {}

  std::vector<UP<IPassElement>> draw() override {
    if (!m_owner)
      return {};
    // Immediate GL calls read Hyprland's clipBox — clear leftovers once per
    // frame so stale values can't eat a ring edge.
    g_pHyprRenderer->m_renderData.clipBox = CBox();
    m_owner->paint();
    return {};
  }

  // The painter draws the blurred backdrop and the strip band. Hyprland
  // refreshes the live-blur framebuffer only for elements reporting true;
  // false → stale blur residue.
  bool needsLiveBlur() override { return m_owner && m_owner->blurEnabled(); }
  bool needsPrecomputeBlur() override { return false; }
  // Occlusion culling must stay off while the overview is up: the painter
  // draws window content at PREVIEW geometry, and simplify() subtracts
  // opaque regions from later elements' damage — any stray accounting would
  // empty the frame over the real, often monitor-filling window rects. The
  // overlay is one visual unit: every presented frame repaints it
  // top-to-bottom anyway, so skipping simplify() adds no extra frames.
  bool disableSimplification() override { return true; }
  bool undiscardable() override { return true; }
  const char *passName() override { return "GloviewPainter"; }
  ePassElementType type() override { return EK_CUSTOM; }
  std::optional<CBox> boundingBox() override {
    const auto m = m_owner ? m_owner->monitor() : nullptr;
    if (!m)
      return std::nullopt;
    return CBox{{}, m->m_size};
  }

private:
  Overview *m_owner = nullptr;
};

} // namespace

// Frame entry (BUILD time, RENDER_LAST_MOMENT — after the top/overlay layers,
// so the overview paints over bars). Everything that MUTATES state happens
// here: clocks advance, hover/sync refresh, the workspace follow runs. Then
// exactly one PainterPass is added; all drawing happens later, in its
// execution.
void Overview::renderStage(eRenderStage stage) {
  if (!m_active)
    return;
  const auto rm = g_pHyprRenderer->m_renderData.pMonitor.lock();
  const auto m = m_monitor.lock();
  if (!rm || !m || rm != m)
    return;

  if (stage != RENDER_LAST_MOMENT)
    return;

  updateAnimation();
  if (!m_active)
    return;

  // Frame trace (debug_logs=1). One line per animated frame, correlating the
  // animation state with what Hyprland's render path was doing at
  // LAST_MOMENT time — the primary live-debugging tool (see CANDIDATES.md
  // for the correlation protocols built on it).
  {
    const double e  = eased();
    const float  k  = static_cast<float>(e);
    const float  kk = m_opening ? k : std::pow(k, 0.45F);
    const auto   sol = rm->m_solitaryClient.lock();
    const bool   blurOK =
        m_blur.valid && m_blur.fb && m_blur.fb->isAllocated() &&
        m_blur.fb->getTexture() && m_blur.fb->getTexture()->ok();
    // Damage footprint: extents + rect count — empty-flag alone hid
    // partial-damage frames (a caret-sized region renders the terminal crisp
    // over an otherwise-stale buffer: the flash signature).
    const auto ext = g_pHyprRenderer->m_renderData.damage.copy().getExtents();
    debug::dbg("F t=+" +
        std::to_string(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m_openStamp)
                .count()) +
        "ms open=" + std::to_string(m_opening) +
        " prog=" + std::to_string(m_progress).substr(0, 5) +
        " kk=" + std::to_string(kk).substr(0, 6) +
        " frost=" + std::to_string(1.0 - e).substr(0, 6) +
        " blurOK=" + std::to_string(blurOK) +
        " bdDrawn=" + std::to_string(m_backdropDrawn) +
        " soli=" + std::to_string((bool)sol) +
        " dso=" + std::to_string(rm->m_directScanoutIsActive) +
        " dmg=" + std::to_string(ext.w) + "x" + std::to_string(ext.h) +
        " rects=" +
        std::to_string(g_pHyprRenderer->m_renderData.damage.copy().getRects().size()) +
        " tiles=" + std::to_string(m_tiles.size()) +
        " strip=" + std::to_string(m_strip.size()) +
        " ws=" +
        std::to_string(m_workspace.lock() ? m_workspace.lock()->m_id : -1) +
        " liveWs=" +
        std::to_string(m->m_activeWorkspace ? m->m_activeWorkspace->m_id : -1) +
        " drag=" + std::to_string((int)m_drag.press) + "/" +
        std::to_string(m_drag.lifted) + "/" + std::to_string(m_drag.idx) +
        " sfx=" + std::to_string(m_swapfx.size()) +
        " dir=" + std::to_string(m_wsSlideDir) +
        " gh=" + std::to_string(m_ghosts.size()) +
        " pop=" + std::to_string(!m_rebuildClock.done(m_entry.ms)));
  }

  updateHover();  // keep hover fresh even when the pointer is warped, not moved
  syncTiles();    // window opened/closed/moved on this workspace → reflow
  updateSnapshots(); // snapshot mode: grab any tile/strip window we don't
                     // have a frozen texture for yet (cheap: contains() +
                     // refcount)

  // Live workspace changed underneath us — a passthrough keybind we don't
  // intercept (`workspace N`, a waybar/widget click, anything outside
  // gloview). Without this the overview sat frozen on stale state, desynced
  // from what's actually on screen.
  if (m_opening && m->m_activeWorkspace != m_liveWsAtOpen.lock()) {
    m_liveWsAtOpen = m->m_activeWorkspace;
    if (cfg::behavior.exit_on_switch != 0) {
      m_workspace = m->m_activeWorkspace; // accept the external switch so
                                          // deactivate() doesn't revert it
      close();
    } else if (!showAllWorkspaces()) {
      // Follow along: treat it like the user clicked that workspace's card.
      // MUST go through captureCurrentBoxes + replayReflow (startTileGlide):
      // a bare rebuild reset Tile.appear to 1 and left stale ghosts — the
      // "tiles jerk then settle" on ctrl-jump and cross-workspace LMB drops.
      debug::dbg("WSFOLLOW ->" + std::to_string(m->m_activeWorkspace->m_id) +
          " tiles=" + std::to_string(m_tiles.size()));
      if (const auto prev = m_workspace.lock())
        m_wsSlideDir = m->m_activeWorkspace->m_id > prev->m_id ? 1 : -1;
      m_workspace = m->m_activeWorkspace;
      const auto shown = captureCurrentBoxes();
      buildTiles();
      buildStrip();
      layoutTiles();
      startTileGlide(shown);
      // Do NOT invalidate the blur cache here — it depends only on the
      // backdrop SOURCE (frozen wallpaper layers or a live mpv texture), not
      // on which workspace is displayed; the per-frame source-identity check
      // in renderBackdrop() handles genuine changes.
      if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
        m_selected = m_tiles.empty() ? -1 : 0;
      damage();
    } else {
      // All-workspaces (expo): the grid already spans every workspace, so a
      // live switch only changes which strip card is highlighted "active".
      buildStrip();
      damage();
    }
  }

  g_pHyprRenderer->m_renderPass.add(makeUnique<PainterPass>(this));

  // While ANY of our clocks runs, keep forcing full-monitor frames: foreign
  // half-damage frames (the real workspace-slide animation schedules them)
  // otherwise interleave and CPreBlur rebuilds only half of m_blurFB —
  // sampled regions then split by a straight line into different eras (the
  // "tile divided in half, one side brighter" artifact).
  {
    const bool busy = secondaryAnimsActive() ||
                      !m_tileClock.done(glideDur()) || m_newCardAnim ||
                      m_drag.lifted || !m_pulses.empty() ||
                      !m_swapfx.empty() ||
                      (m_opening ? m_progress < 1.0 : m_progress > 0.0);
    if (busy)
      m->m_forceFullFrames = std::max(m->m_forceFullFrames, 1);
  }
}

// ---- the painter (EXECUTION time) -------------------------------------------

// Z-slots, top to bottom. Everything reads Model/Clocks/Pixels and mutates
// nothing: clocks advanced in updateAnimation (build), teardown runs in the
// tail. Anything that sits ON TOP of a piece of content draws AFTER it —
// that single rule replaces the entire phase machinery.
//
//   Z0 backdrop      cached blur + dim            (backdrop.cpp)
//   Z1 grid tiles    chrome/frost → ghosts → mains → ✕ → pulses
//                    (tile_view.cpp, fx.cpp)
//   Z2 strip         band/cards/labels → thumbnails → hints/✕ → pulses
//                    (strip_view.cpp, fx.cpp)
//   Z3 drag          chrome → content              (fx.cpp)
//   Z4 above-layers  opted-in TOP/OVERLAY surfaces (fx.cpp)
//   Z5 cursor        HW/SW module                  (cursor.cpp)
//   tail             animation re-arm + close teardown
void Overview::paint() {
  renderBackdrop(); // Z0

  renderPreviews(); // Z1
  renderGhosts();
  renderMainWindows();
  renderTileButtons();
  renderPulses(false /* grid slots */);

  renderStrip(); // Z2
  renderStripWindows();
  renderStripButtons();
  renderPulses(true /* strip slots */);
  renderSwapFX(); // Z2.5: drag/swap flights, above the strip

  const bool dragging =
      draggedTile() >= 0 ||
      (m_drag.press == model::Drag::Press::StripCard && m_drag.lifted) ||
      (m_drag.press == model::Drag::Press::StripWin && !m_drag.win.expired());
  if (dragging) { // Z3
    renderDragTile();
    renderDragWindow();
  }

  renderAboveLayers(); // Z4

  renderCursorOnTop(); // Z5: HW cursor is a KMS plane (always on top, zero
                       // framebuffer writes); only the SW fallback draws here.

  rearmanim(); // tail: re-arm the animation loop AFTER the frame drew —
               // damage() during BUILD landed on the current frame's already-
               // snapshotted region and was consumed by its commit, leaving
               // the next frame with whatever unrelated damage happened
               // along (the historical "black flash" frames).
  finishPendingDeactivate(); // close teardown, after everything painted
}

// Z5: delegate to the cursor module (see cursor.hpp for the HW/SW split).
void Overview::renderCursorOnTop() const {
  const auto m = m_monitor.lock();
  if (!m)
    return;
  m_cursor.renderOnTop(m, cfg::blur.backdrop.get(1.0));
}

} // namespace gloview
