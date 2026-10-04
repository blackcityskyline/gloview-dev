#include "window_content.hpp"

#include <algorithm>
#include <any>
#include <chrono>
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
#include "../config/config.hpp"
#include "../debug/log.hpp"
#include "gl_util.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

// Real opacity: active/inactive rule x FULLSCREEN/LAYOUT fade; workspace-
// switch and FADE alphas excluded — they compound with the caller's fade.
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

// roundPx is the corner radius the CONTENT carries, in monitor pixels, and
// nothing else: matching it to the ring drawn around the tile is the caller's
// job (contentRadiusPx).
//
// `contain` fits the window WHOLE inside destPx instead of covering it. Covering
// requires destPx to carry the window's own aspect — a window covering a box of
// another aspect sticks out past the ring far enough that the corner arcs cannot
// meet. Contained content never sticks out, so a fixed-aspect box is safe; it
// leaves a margin, and the caller's card surface is what fills it.
void renderWindowLive(const PHLWINDOW &w, const PHLMONITOR &mon,
                      const CBox &destPx, const CBox &clipPx, float alpha,
                      const Time::steady_tp &when, int roundPx,
                      float roundingPower, bool settled, bool contain) {
  if (!w || !mon || !w->m_isMapped || !w->wlSurface() ||
      !w->wlSurface()->resource())
    return;
  if (!(destPx.w > 0 && destPx.h > 0))
    return;

  w->wlSurface()->m_fillIgnoreSmall = true;

  // goal(), not value(): value() mid-resize scales only part of destPx.
  const auto pos  = w->positionAnimation()->goal();
  const auto size = w->sizeAnimation()->goal();
  const float logicalW = std::max((float)size.x, 5.F);
  const float logicalH = std::max((float)size.y, 5.F);
  // Over-cover from the CENTER: fit-exact lets the opaque backing show as seams.
  // Contained content sits inside the box, so the seam pad would only shrink it.
  const float pad = (settled || contain) ? 0.0F : kSeamPadPx;
  const float sW  = (static_cast<float>(destPx.w) + pad) /
                   std::max(logicalW * mon->m_scale, 5.F);
  const float sH  = (static_cast<float>(destPx.h) + pad) /
                   std::max(logicalH * mon->m_scale, 5.F);
  const float scaleMod = contain ? std::min(sW, sH) : std::max(sW, sH);
  if (!(scaleMod > 0.F))
    return;

  const Vector2D logicalTL = pos + w->m_floatingOffset;
  const Vector2D scaledTL  = (logicalTL - mon->m_position) * mon->m_scale;
  const Vector2D scaledSize   = Vector2D(logicalW, logicalH) * mon->m_scale;
  const Vector2D winCenter    = scaledTL + scaledSize / 2.0;
  const Vector2D destCenterPx = destPx.pos() + destPx.size() / 2.0;
  const Vector2D translate    = destCenterPx / scaleMod - winCenter;

  Render::SRenderModifData modif;
  modif.modifs.push_back(
      {Render::SRenderModifData::eRenderModifType::RMOD_TYPE_TRANSLATE,
       std::any(translate)});
  modif.modifs.push_back(
      {Render::SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE,
       std::any(scaleMod)});
  modif.enabled = true;

  // Never m_renderPass.add(): the painter iterates that vector right now.
  Render::SRenderModifData savedModif =
      g_pHyprRenderer->m_renderData.renderModif;
  g_pHyprRenderer->m_renderData.renderModif = modif;
  Hyprutils::Utils::CScopeGuard reset([&] {
    g_pHyprRenderer->m_renderData.renderModif = savedModif;
  });

  CSurfacePassElement::SRenderData data{};
  data.pMonitor = mon;
  data.when     = when;
  data.pos      = logicalTL;
  data.w        = std::max(size.x, 5.0);
  data.h        = std::max(size.y, 5.0);
  data.surface  = w->wlSurface()->resource();
  data.dontRound      = false;
  data.fadeAlpha      = windowRealAlpha(w, mon);
  data.alpha          = std::clamp(alpha, 0.F, 1.F);
  data.decorate       = false;
  data.rounding       = roundPx;
  data.roundingPower  = roundingPower;
  data.blur           = false;
  data.pWindow        = w;
  data.clipBox        = clipPx;
  data.squishOversized = true;
  data.surfaceCounter = 0;

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
