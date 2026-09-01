#include "window_content.hpp"

#include <algorithm>
#include <any>
#include <cmath>

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include "../overview.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

// Real, current per-window opacity — mirrors IHyprRenderer::renderWindow's
// renderdata.alpha / renderdata.fadeAlpha (Renderer.cpp): the ACTIVE/INACTIVE
// opacity windowrule (or 1 if the "opaque" rule forces it), times the
// FADE/FULLSCREEN/LAYOUT fade animations. Deliberately excludes the
// workspace-switch-only components (WINDOW_ALPHA_MOVE_TO/FROM_WORKSPACE, and
// the workspace's own m_alpha) — those are real-desktop transition artifacts
// that would otherwise make an off-workspace window's PREVIEW flicker/fade in
// lockstep with a switch animation it isn't actually part of.
//
// The active/inactive component is recomputed here (matching
// CWindow::updateDecorationValues()'s own formula, Window.cpp) instead of
// read from the window's animated WINDOW_ALPHA_ACTIVE slot, because that slot
// is only refreshed on a real focus-change EVENT. A tile whose window sits on
// a hidden workspace never gets one while the overview is up — syncFocus()
// deliberately only calls fullWindowFocus() for a tile on the monitor's REAL
// active workspace. Keying the recompute off whether `w` is REALLY the live,
// on-screen focused window right now (true only on the monitor's real active
// workspace) means every hidden-workspace tile unconditionally renders with
// its inactive-opacity windowrule — the only value that can ever be correct
// for a window nothing is actually looking at.
//
// WINDOW_ALPHA_FADE is deliberately excluded: CSurfacePassElement multiplies
// data.alpha * data.fadeAlpha, and any FADE contribution would compound with
// the caller's own alpha parameter. The preview fade axis belongs solely to
// the caller.
float windowRealAlpha(const PHLWINDOW &w, const PHLMONITOR &mon) {
  if (!w)
    return 1.0F;
  float active = 1.0F;
  if (w->m_ruleApplicator) {
    static auto PACTIVEALPHA =
        CConfigValue<Config::FLOAT>("decoration:active_opacity");
    static auto PINACTIVEALPHA =
        CConfigValue<Config::FLOAT>("decoration:inactive_opacity");
    static auto PFULLSCREENALPHA =
        CConfigValue<Config::FLOAT>("decoration:fullscreen_opacity");
    if (Fullscreen::controller()->isFullscreen(w,
                                               Fullscreen::FSMODE_FULLSCREEN)) {
      active =
          w->m_ruleApplicator->alphaFullscreen().valueOrDefault().applyAlpha(
              *PFULLSCREENALPHA);
    } else {
      const bool reallyFocused = mon &&
                                 w->m_workspace == mon->m_activeWorkspace &&
                                 w == Desktop::focusState()->window();
      active = reallyFocused
                   ? w->m_ruleApplicator->alpha().valueOrDefault().applyAlpha(
                         *PACTIVEALPHA)
                   : w->m_ruleApplicator->alphaInactive()
                         .valueOrDefault()
                         .applyAlpha(*PINACTIVEALPHA);
    }
    if (w->m_ruleApplicator->opaque().valueOrDefault())
      active = 1.0F;
  }
  const float fade = w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) *
                     w->alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT);
  return std::clamp(active * fade, 0.0F, 1.0F);
}

} // namespace

// The immediate window-content leaf: ONE breadthfirst walk over the window's
// surface tree, each node executed synchronously via the public
// IHyprRenderer::draw(SRenderData, CRegion) (Renderer.cpp:859, pinned 0.56.2)
// — the exact preDrawSurface/drawSurface machinery a queued element would run
// (per-surface alpha modifiers, calculateUVForSurface small/viewporter
// corrections, presentFeedback frame callbacks so clients don't stall, blend
// state, async dmabuf buffer tracking), without touching m_renderPass.
//
// The translate+scale render-modif maps the window's real geometry into
// destPx; it is applied directly to m_renderData.renderModif — precisely what
// the queued route's CRendererHintsPassElement does (ElementRenderer.cpp) —
// and restored after the last node. Call only from pass EXECUTION (inside the
// painter), where currentFB/projection are live.
void renderWindowLive(const PHLWINDOW &w, const PHLMONITOR &mon,
                      const CBox &destPx, const CBox &clipPx, float alpha,
                      const Time::steady_tp &when, int roundPx,
                      float roundingPower) {
  if (!w || !mon || !w->m_isMapped || !w->wlSurface() ||
      !w->wlSurface()->resource())
    return;
  if (!(destPx.w > 0 && destPx.h > 0))
    return;

  // When reported size > committed buffer (CWLSurface::small(): X11 size
  // hints/mid-resize), getTexBox CENTERS it at real size, leaving an
  // uncovered margin. m_fillIgnoreSmall stretches to fill; Hyprland never
  // sets it, so restoreFill() resets on teardown.
  w->wlSurface()->m_fillIgnoreSmall = true;

  // SETTLED goal() geometry, not mid-animation value(): destPx is sized from
  // goal(), so scaling by value() mid-resize fills only part of the box →
  // black side strips. Position cancels in the translate remap below, so only
  // size matters.
  const auto pos  = w->positionAnimation()->goal();
  const auto size = w->sizeAnimation()->goal();
  const float logicalW = std::max((float)size.x, 5.F);
  const float logicalH = std::max((float)size.y, 5.F);
  // Over-cover the slot (fill BOTH axes via max + ~1.5px pad), don't just fit
  // it: a fit-exact scale rounds the surface edge 1-3px inside the box and
  // the opaque backing peeks through as thin dark seams (worst mid
  // open-glide, destPx fractional every frame). Anchored at destPx's CENTER
  // (see translate below) so the extra coverage distributes evenly on all
  // four sides; clipPx trims any remaining overflow.
  const float pad = 0.0F; // TEMP DIAGNOSTIC — was 1.5F
  const float sW  = (static_cast<float>(destPx.w) + pad) /
                   std::max(logicalW * mon->m_scale, 5.F);
  const float sH  = (static_cast<float>(destPx.h) + pad) /
                   std::max(logicalH * mon->m_scale, 5.F);
  const float scaleMod = std::max(sW, sH);
  if (!(scaleMod > 0.F))
    return;

  const Vector2D logicalTL = pos + w->m_floatingOffset;
  const Vector2D scaledTL  = (logicalTL - mon->m_position) * mon->m_scale;
  // Anchor the overcover at the box CENTER, not top-left: the pad above
  // exists to over-cover destPx so no dark seam shows on any edge, but
  // anchoring the scale-up at top-left means the extra ~1.5px only grows
  // toward bottom-right. The content's own rounded-corner clip (computed in
  // the surface's own coordinate frame) then sits up to ~1.5px further out
  // than the border ring (which is drawn exactly at destPx/lb, unpadded) on
  // the bottom and right sides specifically — top-left corner lines up
  // almost perfectly, the other three show a growing mismatch. Anchoring at
  // center instead spreads that same ~1.5px surplus evenly in all four
  // directions (~0.75px each), making the residual content/border corner
  // mismatch small and symmetric across all four corners instead of
  // concentrated on three of them.
  const Vector2D scaledSize   = Vector2D(logicalW, logicalH) * mon->m_scale;
  const Vector2D winCenter    = scaledTL + scaledSize / 2.0;
  const Vector2D destCenterPx = destPx.pos() + destPx.size() / 2.0;
  const Vector2D translate    = destCenterPx / scaleMod - winCenter;

  // The overcover pad above makes the AS-RENDERED content box bigger than
  // destPx by design (avoids dark seams). But content's own rounded-corner
  // clip (rounding.glsl) is a circle whose center sits at
  // (box.topLeft + radius) — growing the box while leaving `radius` (roundPx)
  // unchanged moves that circle's center outward with the box, since the
  // topLeft term grows too. The border ring, drawn separately at the exact
  // (unpadded) destPx/lb with the SAME roundPx, keeps its circle-center fixed
  // at destPx's own corner. The two centers drift apart by exactly the
  // per-side overcover amount — content's corner arc no longer lines up with
  // the border's, showing as a gap (or content visibly extending past where
  // the border's curve should read as the edge).
  //
  // Fix: increase content's OWN rounding radius by the per-side overcover
  // amount. Growing radius by the same delta the topLeft shifted by keeps
  // the circle-center formula (topLeft + radius) exactly where it was —
  // the corner arc is unaffected by the pad, only the flat straight edges
  // (which don't depend on a single corner circle) absorb the extra
  // coverage, which is exactly what the pad was meant to protect there.
  const Vector2D renderedSize = scaledSize * scaleMod;
  const Vector2D overcoverPx  = renderedSize - destPx.size();
  const float roundCompensation =
      std::max(0.0, std::max(overcoverPx.x, overcoverPx.y)) / 2.0F;
  // roundPx == 0 means square corners are intended (preview_round=0, or a
  // caller that wants no rounding at all) — border.glsl's own `radius > 0.0`
  // gate skips ALL corner-arc logic in that case. Adding compensation on top
  // of 0 would introduce UNWANTED rounding on content while the border (not
  // touched by this compensation) correctly stays perfectly square — a new
  // content/border mismatch for anyone who explicitly wants square previews.
  const int contentRoundPx =
      roundPx <= 0 ? 0
                   : roundPx + static_cast<int>(std::lround(roundCompensation));

  Render::SRenderModifData modif;
  modif.modifs.push_back(
      {Render::SRenderModifData::eRenderModifType::RMOD_TYPE_TRANSLATE,
       std::any(translate)});
  modif.modifs.push_back(
      {Render::SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE,
       std::any(scaleMod)});
  modif.enabled = true;

  // Never m_renderPass.add() here: the painter runs while
  // CRenderPass::render iterates the element vector.
  Render::SRenderModifData savedModif =
      g_pHyprRenderer->m_renderData.renderModif;
  g_pHyprRenderer->m_renderData.renderModif = modif;
  Hyprutils::Utils::CScopeGuard reset([&] {
    g_pHyprRenderer->m_renderData.renderModif = savedModif;
  });

  // Do NOT poke damageWindow(w) here: that is a self-sustaining render loop
  // (damage → frame → renderWindowLive → damage …, measured ~20% GPU idle).
  // Liveness comes from the client's own commits and from Overview::damage()
  // while a clock runs.

  CSurfacePassElement::SRenderData data{};
  data.pMonitor = mon;
  data.when     = when;
  data.pos      = logicalTL;
  data.w        = std::max(size.x, 5.0);
  data.h        = std::max(size.y, 5.0);
  data.surface  = w->wlSurface()->resource();
  // Always round the preview: the real-desktop rule (square corners for
  // fullscreen windows) is meaningless for a shrunk-down tile/thumbnail and
  // produced one square-cornered preview among rounded ones.
  data.dontRound      = false;
  data.fadeAlpha      = windowRealAlpha(w, mon);
  data.alpha          = std::clamp(alpha, 0.F, 1.F);
  data.decorate       = false;
  data.rounding       = contentRoundPx;
  data.roundingPower  = roundingPower;
  // Previews never touch Hyprland's blur machinery: the live branch samples
  // currentFB at draw time and bakes ghost copies of sibling previews into
  // every translucent tile's backdrop. Translucent interiors get the frost
  // underlay instead (tile_view.cpp).
  data.blur           = false;
  data.pWindow        = w;
  data.clipBox        = clipPx;
  data.squishOversized = true;
  data.surfaceCounter = 0;

  // Snapshot mode (preview_mode == "snapshot"): one element from the frozen
  // build-time texture instead of the live walk — a fraction of the GPU work
  // (measured ~20% → ~0% Render busy on an iGPU with a video tile), at the
  // cost of a static preview. No captured texture yet (window appeared this
  // frame) → fall through to the live path; updateSnapshots() grabs it next
  // build.
  if (g_overview && g_overview->snapshotMode()) {
    const auto snapIt = g_overview->snapshots().find(w.get());
    if (snapIt != g_overview->snapshots().end() && snapIt->second &&
        snapIt->second->ok()) {
      auto sdata      = data;
      sdata.texture   = snapIt->second;
      sdata.localPos  = Vector2D{};
      sdata.mainSurface = true;
      g_pHyprRenderer->draw(sdata, g_pHyprRenderer->m_renderData.damage);
      return;
    }
  }

  w->wlSurface()->resource()->breadthfirst(
      [&data, &w, &destPx](SP<CWLSurfaceResource> s, const Vector2D &offset,
                           void *) {
        if (!s || !s->m_current.texture || s->m_current.size.x < 1 ||
            s->m_current.size.y < 1)
          return;
        data.localPos    = offset;
        data.texture     = s->m_current.texture;
        data.surface     = s;
        data.mainSurface = s == w->wlSurface()->resource();
        g_pHyprRenderer->draw(data, g_pHyprRenderer->m_renderData.damage);
        data.surfaceCounter++;
      },
      nullptr);
}

} // namespace gloview
