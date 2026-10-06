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
  // The dim is the same material as the blur and rides the same clock: it used to run on
  // the open animation while the blur ran on its own, the two parted halfway, and the exit
  // laid down wrong because of it.
  const double dimK = blurAlpha();
  debug::dbg(std::string("backdrop e=") + std::to_string(e).substr(0, 5) +
      " opening=" + std::to_string(m_opening) +
      " k=" + std::to_string(k).substr(0, 5));

  // Entry fade needs an OPAQUE base built from the backdrop SOURCES: at low k
  // the blend target is currentFB, whose stale wallpaper blinks dark on frame 0.
  // baseIsBlur: the base is already the blurred texture, so the ramp must not blend it
  // again on top of itself.
  bool baseIsBlur = false;
  if (m_opening) {
    // NOT gated on m_blur.valid: valid drives RECOMPUTATION, the FB still holds the last
    // good blur. open() clears valid whenever the cache goes stale, and gating on it left
    // the windowed entry on a sharp composite for its first ~40ms (measured: baseBlur=0
    // on frames 1-2) — the exact gap the blurred base exists to avoid.
    const bool blurCacheReady = m_blur.fb && m_blur.fb->isAllocated() &&
        m_blur.fb->m_size == Vector2D(W, H);
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
    // WHICH base depends on the landing desktop, and it is the whole difference between the
    // two behaviours. WINDOWED: whatever blurred the screen a moment ago was Hyprland's
    // own blur under those windows, and it stops the instant they start flying — so the
    // base must already be blurred, handing over blurred to blurred. A sharp base there
    // leaves the desktop clear for the whole ramp (measured). EMPTY: nothing was blurring
    // it and nothing takes over afterwards, so the SHARP composite is safe, and it is what
    // gives the ramp something to reveal.
    if (cachedUsable && !m_landingEmpty) {
      baseIsBlur = true;
      g_pHyprOpenGL->renderTexture(cachedTex, fullPx, {.a = 1.0F});
    } else {
      const auto oldProj = g_pHyprRenderer->m_renderData.projectionType;
      const auto oldFbSz = g_pHyprRenderer->m_renderData.fbSize;
      g_pHyprRenderer->m_renderData.fbSize = Vector2D(W, H);
      g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
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
        if (stale && stale->ok()) {
          baseIsBlur = true;
          g_pHyprOpenGL->renderTexture(stale, fullPx, {.a = 1.0F});
        }
      }
    }
  }


  if (!blurEnabled()) {
    const auto col = argb(baseCol, dimK);
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
      // No fill. The entry base is already on screen, and misc:background_color is black
      // whenever the wallpaper is a layer surface -- filling here IS the flash.
      g_pHyprOpenGL->renderRect(fullPx, argb(baseCol, dimK), {});
      return;
    }
    m_blur.valid = true;
    m_lastBackdropCapture = std::chrono::steady_clock::now();
  }

  const auto tex = m_blur.fb ? m_blur.fb->getTexture() : nullptr;
  // ENTRY rides its own clock, not the open animation: the open is 300ms of easeout,
  // which puts the blur at ~full strength within one or two frames and reads as a hard
  // cut. The fade outlives the open by design, so the anim pump keeps frames coming
  // (named in secondaryAnimsActive) until it reaches 1.
  const float kk = static_cast<float>(blurAlpha());
  if (tex && tex->ok() && !baseIsBlur) {
    // Measured: blending is OFF here on the frames where the entry lands, so the blit
    // ignores kk and the texture goes down opaque. The blur texture is black on exactly
    // those frames (its source composite was rejected as black), so the frame went black
    // instead of fading in at 3% alpha.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    g_pHyprOpenGL->renderTexture(tex, fullPx, {.a = kk});
  } else if (!baseIsBlur) {
    // Keep the entry base, same reason as the fallback above.
    m_blur.valid = false;
  }

  


  const auto dimCol = argb(baseCol, dimK);
  if (dimCol.a > 0.0)
    g_pHyprOpenGL->renderRect(fullPx, dimCol, {});


}

} // namespace gloview
