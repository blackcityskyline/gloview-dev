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

class PainterPass final : public IPassElement {
public:
  explicit PainterPass(Overview *o) : m_owner(o) {}

  std::vector<UP<IPassElement>> draw() override {
    if (!m_owner)
      return {};
    // stale clipBox clips our geometry
    g_pHyprRenderer->m_renderData.clipBox = CBox();
    m_owner->paint();
    return {};
  }

  // false leaves blur residue
  bool needsLiveBlur() override { return m_owner && m_owner->blurEnabled(); }
  bool needsPrecomputeBlur() override { return false; }
  // simplify() + preview geometry can empty it
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

}

// BUILD time (RENDER_LAST_MOMENT): mutate here, draw in draw().
void Overview::renderStage(eRenderStage stage) {
  if (stage == RENDER_LAST_MOMENT)
    ++m_frameTick;

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

  {
    const double e  = eased();
    const float  k  = static_cast<float>(e);
    const float  kk = m_opening ? k : std::pow(k, 0.45F);
    const auto   sol = rm->m_solitaryClient.lock();
    const bool   blurOK =
        m_blur.valid && m_blur.fb && m_blur.fb->isAllocated() &&
        m_blur.fb->getTexture() && m_blur.fb->getTexture()->ok();
    const auto ext = g_pHyprRenderer->m_renderData.damage.copy().getExtents();
    debug::dbg("F t=+" +
        std::to_string(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m_openStamp)
                .count()) +
        "ms tick=" + std::to_string(m_frameTick) +
        " forceFF=" + std::to_string(rm->m_forceFullFrames) +
        " open=" + std::to_string(m_opening) +
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

  updateHover();
  relayoutCarousel();
  syncTiles();
  updateSnapshots();

  if (m_opening && m->m_activeWorkspace != m_liveWsAtOpen.lock()) {
    m_liveWsAtOpen = m->m_activeWorkspace;
    if (cfg::behavior.exit_on_switch != 0) {
      m_workspace = m->m_activeWorkspace;
      close();
    } else if (!showAllWorkspaces()) {
      // capture + replay reflow, else stale ghosts survive
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
      // no blur invalidation: source id rechecked per frame
      if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
        m_selected = m_tiles.empty() ? -1 : 0;
      damage();
    } else {
      buildStrip();
      damage();
    }
  }

  g_pHyprRenderer->m_renderPass.add(makeUnique<PainterPass>(this));

  // half-damage frames rebuild half the FB
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

// Z-slots, top to bottom: on-top-of-content draws after it.
void Overview::paint() {
  renderBackdrop();

  if (carouselMode()) {
    renderCarousel(); // overlapping cards: content interleaves per card
    renderGhosts();
  } else {
    renderPreviews();
    renderGhosts();
    renderMainWindows();
    renderTileButtons();
    renderPulses(false);
  }

  renderStrip();
  renderStripWindows();
  renderStripButtons();
  renderPulses(true);
  renderSwapFX();

  const bool dragging =
      draggedTile() >= 0 ||
      (m_drag.press == model::Drag::Press::StripWin && m_drag.lifted);
  if (dragging) {
    renderDragTile();
    renderDragWindow();
  }

  renderAboveLayers();

  renderCursorOnTop();

  rearmanim(); // tail: damage() here hits already-committed damage
  finishPendingDeactivate();
}

void Overview::renderCursorOnTop() const {
  const auto m = m_monitor.lock();
  if (!m)
    return;
  m_cursor.renderOnTop(m, cfg::blur.backdrop.get(1.0));
}

}
