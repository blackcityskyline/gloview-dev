#include <algorithm>
#include <chrono>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include "gl_util.hpp"
#include "../config/config.hpp"
#include "../debug/log.hpp"
#include "../overview.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

// Never sample currentFB: a fullscreen window on the active workspace renders
// through the "solitary client" fast path, and an idle workspace leaves it stale.
// Source: a fullscreen video client (live, never cached), the wallpaper, or a
// private FBO of BACKGROUND/BOTTOM layer-shell surfaces. Blur cache is PURE.

SP<Render::ITexture> Overview::backdropSource(bool &live) const {
  live = false;
  const auto m = m_monitor.lock();
  if (!m)
    return nullptr;
  const auto fsBg = cfg::blur.fullscreen_background;
  if (fsBg) {
    const auto ws = m_workspace.lock();
    for (const auto &w : Desktop::windowState()->windows()) {
      if (!w || w->isHidden() || !w->m_isMapped || w->m_workspace != ws)
        continue;
      if (!Fullscreen::controller()->isFullscreen(w))
        continue;
      if (w->m_class != "mpv")
        continue;
      const auto surf = w->wlSurface() ? w->wlSurface()->resource()
                                       : SP<CWLSurfaceResource>{};
      if (surf && surf->m_current.texture && surf->m_current.texture->ok() &&
          surf->m_current.size.x > 0 && surf->m_current.size.y > 0) {
        live = true;
        return surf->m_current.texture;
      }
    }
  }
  return nullptr;
}

SP<Render::ITexture> Overview::renderBackdropSource(int W, int H) const {
  const auto m = m_monitor.lock();
  if (!m || !g_pHyprOpenGL || !g_pHyprRenderer)
    return nullptr;

  bool needRedraw = false;
  if (!m_backdropSrcFB) {
    m_backdropSrcFB = g_pHyprRenderer->createFB("gloview backdrop");
    needRedraw = true;
  }
  if (!m_backdropSrcFB->isAllocated() ||
      m_backdropSrcFB->m_size != Vector2D(W, H)) {
    m_backdropSrcFB->alloc(W, H);
    needRedraw = true;
    m_backdropDrawn = false;
  }

  if (m_backdropDrawn && !needRedraw)
    return m_backdropSrcFB->getTexture();

  // A layer texture whose identity just changed is still being re-created: compositing it
  // on that frame yields a black source, and a black source means a black entry frame.
  // Hold the previous composite until the texture has settled.
  {
    const void *tex = nullptr;
    for (const int li : {0, 1})
      for (const auto &lr : m->m_layerSurfaceLayers[li]) {
        const auto layer = lr.lock();
        if (!layer || !layer->m_mapped || !layer->visible() ||
            !layer->wlSurface() || !layer->wlSurface()->resource())
          continue;
        const auto surf = layer->wlSurface()->resource();
        if (!surf->m_current.texture || !surf->m_current.texture->ok())
          continue;
        tex = surf->m_current.texture.get();
        break;
      }
    if (tex && tex == m_lastLayerTex)
      m_layerStable++;
    else {
      m_lastLayerTex = tex;
      m_layerStable = 0;
    }
    if (!needRedraw && m_layerStable < 3 && m_backdropSrcFB->isAllocated()) {
      m_backdropDrawn = true;
      return m_backdropSrcFB->getTexture();
    }
  }

  const auto oldFB = g_pHyprRenderer->m_renderData.currentFB;
  GLint oldVp[4];
  glGetIntegerv(GL_VIEWPORT, oldVp);
  // As updateSnapshots does for its private FB. Measured NOT the black-backdrop cause.
  const auto oldTransformDmg = g_pHyprRenderer->m_renderData.transformDamage;
  const auto oldModif        = g_pHyprRenderer->m_renderData.renderModif;
  g_pHyprRenderer->m_renderData.transformDamage = false;
  g_pHyprRenderer->m_renderData.renderModif     = Render::SRenderModifData{};
  m_backdropSrcFB->bind();
  g_pHyprRenderer->m_renderData.currentFB = m_backdropSrcFB;
  g_pHyprOpenGL->setViewport(0, 0, W, H);
  Hyprutils::Utils::CScopeGuard guard([&] {
    g_pHyprRenderer->m_renderData.transformDamage = oldTransformDmg;
    g_pHyprRenderer->m_renderData.renderModif     = oldModif;
    g_pHyprRenderer->m_renderData.currentFB = oldFB;
    if (oldFB)
      oldFB->bind();
    g_pHyprOpenGL->setViewport(oldVp[0], oldVp[1], oldVp[2], oldVp[3]);
  });

  // Only on a fresh FB. On a redraw the previous composite is a usable picture; filling
  // black here is what turns "the layer texture was not ready this frame" into a frame with
  // no source at all.
  if (needRedraw) {
    static auto PBG = CConfigValue<Config::INTEGER>("misc:background_color");
    g_pHyprOpenGL->renderRect(CBox(0, 0, W, H), argb(*PBG, 1.0), {});
  }

  if (m->m_background && m->m_background->ok()) {
    const double MONRATIO = static_cast<double>(W) / H;
    const double WPRATIO =
        m->m_background->m_size.x / m->m_background->m_size.y;
    double scale;
    Vector2D origin;
    if (MONRATIO > WPRATIO) {
      scale = static_cast<double>(W) / m->m_background->m_size.x;
      origin.y = (H - m->m_background->m_size.y * scale) / 2.0;
    } else {
      scale = static_cast<double>(H) / m->m_background->m_size.y;
      origin.x = (W - m->m_background->m_size.x * scale) / 2.0;
    }
    g_pHyprOpenGL->renderTexture(m->m_background,
                                 CBox{origin.x, origin.y,
                                      m->m_background->m_size.x * scale,
                                      m->m_background->m_size.y * scale},
                                 {.a = 1.0F});
  }

  const double s = m->m_scale;
  int drew = 0;
  for (const int li : {0, 1}) { // LAYER_BACKGROUND, LAYER_BOTTOM
    for (const auto &lr : m->m_layerSurfaceLayers[li]) {
      const auto layer = lr.lock();
      if (!layer || !layer->m_mapped || !layer->visible() ||
          !layer->wlSurface() || !layer->wlSurface()->resource())
        continue;
      const auto surf = layer->wlSurface()->resource();
      if (!surf->m_current.texture || !surf->m_current.texture->ok())
        continue;
      const auto pos =
          layer->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
      const auto sz = layer->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
      if (sz.x < 1.0 || sz.y < 1.0)
        continue;
      const float a = layer->alpha()[Desktop::View::LS_ALPHA_FADE]->value();
      g_pHyprOpenGL->renderTexture(
          surf->m_current.texture,
          pxb(CBox{pos.x - m->m_position.x, pos.y - m->m_position.y, sz.x,
                   sz.y},
              s),
          {.a = a});
      drew++;
    }
  }
  // Measured: the wallpaper layer texture is sometimes mid-recreation — the draw happens,
  // the viewport is right, blending is on — and the composite still comes out black.
  // Cached, that black composite became the entry base and the frame went black. One pixel
  // decides; a frame without a source is reported rather than painted over the desktop.
  unsigned char q[4] = {0, 0, 0, 0};
  glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
  if (!drew || (q[0] < 8 && q[1] < 8 && q[2] < 8)) {
    m_backdropDrawn = false;
    return nullptr;
  }

  m_backdropDrawn = true;
  return m_backdropSrcFB->getTexture();
}

void Overview::renderBackdrop() const {
  const auto m = m_monitor.lock();
  if (!m || !g_pHyprRenderer || !g_pHyprOpenGL)
    return;
  const double s      = m->m_scale;
  const auto fullPx   = pxb(CBox(0, 0, m->m_size.x, m->m_size.y), s);
  const int W         = m->m_pixelSize.x;
  const int H         = m->m_pixelSize.y;
  const double e      = eased();
  const auto baseCol  = cfg::blur.backdrop.get();

  const float k = static_cast<float>(e);
  debug::dbg(std::string("backdrop e=") + std::to_string(e).substr(0, 5) +
      " opening=" + std::to_string(m_opening) +
      " k=" + std::to_string(k).substr(0, 5));

  // Entry fade needs an OPAQUE base built from the backdrop SOURCES: at low k
  // the blend target is currentFB, whose stale wallpaper blinks dark on frame 0.
  if (m_opening) {
    // valid, not merely allocated: open() clears m_blur.valid when the cache goes stale.
    const bool blurCacheReady = m_blur.valid && m_blur.fb &&
        m_blur.fb->isAllocated() && m_blur.fb->m_size == Vector2D(W, H);
    const auto cachedTex = blurCacheReady ? m_blur.fb->getTexture() : nullptr;
    // Reading m_blur.fb directly returns multisample garbage, so the cache is sampled
    // through a 1x1 framebuffer instead. Measured: the cache is black on the first frame
    // after a stale open, and it is the entry base, so the frame goes black.
    static auto probeFB = g_pHyprRenderer->createFB("gloview cache probe");
    bool cachedUsable = false;
    unsigned char q[4] = {0, 0, 0, 0};
    if (cachedTex && cachedTex->ok() && probeFB) {
      if (!probeFB->isAllocated())
        probeFB->alloc(1, 1);
      GLint bnd = 0;
      GLint vp[4] = {0, 0, 0, 0};
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bnd);
      glGetIntegerv(GL_VIEWPORT, vp);
      probeFB->bind();
      g_pHyprOpenGL->setViewport(0, 0, 1, 1);
      // Three samples: a single one can land in a legitimately dark corner.
      const CBox spots[3] = {CBox(0, 0, 1, 1), CBox(0.25F, 0.25F, 1, 1),
                             CBox(0.75F, 0.75F, 1, 1)};
      int lit = 0;
      for (const auto &spot : spots) {
        g_pHyprOpenGL->renderTexture(cachedTex, spot, {.a = 1.0F});
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
        if (!(q[0] < 8 && q[1] < 8 && q[2] < 8))
          lit++;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, bnd);
      glViewport(vp[0], vp[1], vp[2], vp[3]);
      cachedUsable = lit > 0;
    }
    if (cachedUsable) {
      g_pHyprOpenGL->renderTexture(cachedTex, fullPx, {.a = 1.0F});
    } else {

      const auto oldProj = g_pHyprRenderer->m_renderData.projectionType;
      const auto oldFbSz = g_pHyprRenderer->m_renderData.fbSize;
      g_pHyprRenderer->m_renderData.fbSize = Vector2D(W, H);
      g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
      // No usable cache: composite the sources — the old flatFill fallback was black here.
      const auto comp = renderBackdropSource(W, H);
      g_pHyprRenderer->m_renderData.fbSize = oldFbSz;
      g_pHyprRenderer->setProjectionType(oldProj);
      if (comp && comp->ok()) {
        g_pHyprOpenGL->renderTexture(comp, fullPx, {.a = 1.0F});
      } else if (m_blur.fb && m_blur.fb->isAllocated()) {
        // No source this frame. The stale blur FB still holds the previous, correct
        // backdrop — an outdated frame beats a black one, and m_blur.valid only gates
        // recomputation, not the contents.
        const auto stale = m_blur.fb->getTexture();
        if (stale && stale->ok())
          g_pHyprOpenGL->renderTexture(stale, fullPx, {.a = 1.0F});
      }
    }
  }


  if (!blurEnabled()) {
    const auto col = argb(baseCol, e);
    if (col.a <= 0.0)
      return;
    g_pHyprOpenGL->renderRect(fullPx, col, {});
    return;
  }

  bool liveSrc = false;
  auto src = backdropSource(liveSrc);
  if (!src) {
    const auto oldProj = g_pHyprRenderer->m_renderData.projectionType;
    const auto oldFbSz = g_pHyprRenderer->m_renderData.fbSize;
    g_pHyprRenderer->m_renderData.fbSize = Vector2D(W, H);
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    src = renderBackdropSource(W, H);
    g_pHyprRenderer->m_renderData.fbSize = oldFbSz;
    g_pHyprRenderer->setProjectionType(oldProj);
  }

  if (!src || k <= 0.0F)
    return;

  const void *srcId = src ? src.get() : nullptr;
  const int   cPasses  = blurPasses();
  const int   cSize    = blurSize();
  const int   cRes     = blurResolution();
  const float cStrength = blurStrength();
  if (liveSrc || !m_blur.matches(srcId, cPasses, cSize, cRes, cStrength))
    m_blur.invalidate();
  m_blur.srcId     = srcId;
  m_blur.passes    = cPasses;
  m_blur.sizePx    = cSize;
  m_blur.resolution = cRes;
  m_blur.strength  = cStrength;

  if (!m_blur.valid || !m_blur.fb || !m_blur.fb->isAllocated() ||
      m_blur.fb->m_size != Vector2D(W, H)) {
    if (!m_blur.fb)
      m_blur.fb = g_pHyprRenderer->createFB("gloview blur");
    if (!m_blur.fb->isAllocated() || m_blur.fb->m_size != Vector2D(W, H))
      m_blur.fb->alloc(W, H);

    const auto oldProjType = g_pHyprRenderer->m_renderData.projectionType;
    const auto oldFbSize   = g_pHyprRenderer->m_renderData.fbSize;
    g_pHyprRenderer->m_renderData.fbSize = Vector2D(W, H);
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);

    m_blurFilter.prepare(cPasses, static_cast<float>(cSize), cRes, cStrength);
    const bool ok = src && m_blurFilter.render(src, m_blur.fb, W, H);

    g_pHyprRenderer->m_renderData.fbSize = oldFbSize;
    g_pHyprRenderer->setProjectionType(oldProjType);
    const auto PMON = g_pHyprRenderer->m_renderData.pMonitor;
    g_pHyprOpenGL->setViewport(0, 0, PMON ? (int)PMON->m_pixelSize.x : W,
                               PMON ? (int)PMON->m_pixelSize.y : H);

    if (!ok) {
      // Never leave currentFB exposed: it can hold a solitary fullscreen window.
      m_blur.valid = false;
      debug::dbg("backdrop FALLBACK: blur unavailable / no source");
      static auto PBG = CConfigValue<Config::INTEGER>("misc:background_color");
      g_pHyprOpenGL->renderRect(fullPx, argb(*PBG, 1.0), {});
      g_pHyprOpenGL->renderRect(fullPx, argb(baseCol, e), {});
      return;
    }
    m_blur.valid = true;
    m_lastBackdropCapture = std::chrono::steady_clock::now();
  }

  const auto tex = m_blur.fb ? m_blur.fb->getTexture() : nullptr;
  const float kk = m_opening ? k : std::pow(k, 0.45F);
  if (tex && tex->ok()) {
    g_pHyprOpenGL->renderTexture(tex, fullPx, {.a = kk});
  } else {
    m_blur.valid = false;
    static auto PBG2 = CConfigValue<Config::INTEGER>("misc:background_color");
    g_pHyprOpenGL->renderRect(fullPx, argb(*PBG2, 1.0), {});
  }


  const auto dimCol = argb(baseCol, e);
  if (dimCol.a > 0.0)
    g_pHyprOpenGL->renderRect(fullPx, dimCol, {});
}

} // namespace gloview
