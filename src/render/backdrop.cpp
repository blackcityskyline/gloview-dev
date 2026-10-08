#include <algorithm>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Color.hpp>
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
// Source: the wallpaper, or a private FBO of BACKGROUND/BOTTOM layer-shell
// surfaces. Blur cache is PURE.

SP<Render::ITexture> Overview::renderBackdropSource(int W, int H) const {
  const auto m = m_monitor.lock();
  if (!m || !g_pHyprOpenGL || !g_pHyprRenderer)
    return nullptr;
  const auto size = Vector2D(W, H);
  const auto fit = [size](const SP<Render::IFramebuffer> &fb) {
    return fb && fb->isAllocated() && fb->m_size == size;
  };
  if (!m_backdropSrcFB)
    m_backdropSrcFB = g_pHyprRenderer->createFB("gloview backdrop");
  if (!fit(m_backdropSrcFB)) {
    m_backdropSrcFB->alloc(W, H);
    m_backdropDrawn = false;
  }
  if (!fit(m_backdropSrcFB))
    return nullptr;

  // The layer texture the committed composite was made from, and the one live right now.
  // The composite is kept indefinitely and compared on every open; there is no timer.
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
  // Live source? A video wallpaper replaces its texture on every frame. Require several
  // changes IN A ROW so a single compositor re-creation still goes through the safe path.
  static constexpr int kAnimatedStreak = 4;
  if (tex != m_lastObservedTex) {
    m_srcAnimStreak   = m_lastObservedTex ? m_srcAnimStreak + 1 : 0;
    m_lastObservedTex = tex;
  } else {
    m_srcAnimStreak = 0;
  }
  m_srcAnimated = m_srcAnimStreak >= kAnimatedStreak;

  // A live source is cached ONCE and then held: the overview shows a single frozen frame of
  // the video wallpaper. Re-making it every frame buys nothing here -- the backdrop is blurred
  // and static for the whole open -- and costs a composite plus a blur every frame.
  if (m_backdropDrawn && (m_srcAnimated || (tex && tex == m_backdropSrcTex)))
    return m_backdropSrcFB->getTexture();

  // The source changed. Rebuild it into a SCRATCH framebuffer and let its CONTENT decide,
  // because a frame count cannot tell these apart:
  //   - the user changed the wallpaper and the new image is already there -> adopt at once,
  //     holding a fixed number of frames is what showed the previous wallpaper for a flash
  //     after every change;
  //   - the compositor is mid-recreation and this texture still reads black -> keep the good
  //     composite, which is the whole reason it is kept.
  if (!m_backdropScratchFB)
    m_backdropScratchFB = g_pHyprRenderer->createFB("gloview backdrop scratch");
  if (!fit(m_backdropScratchFB))
    m_backdropScratchFB->alloc(W, H);
  if (!fit(m_backdropScratchFB))
    return m_backdropDrawn ? m_backdropSrcFB->getTexture() : nullptr;

  const auto oldFB = g_pHyprRenderer->m_renderData.currentFB;
  GLint oldVp[4];
  glGetIntegerv(GL_VIEWPORT, oldVp);
  const auto oldTransformDmg = g_pHyprRenderer->m_renderData.transformDamage;
  const auto oldModif        = g_pHyprRenderer->m_renderData.renderModif;
  g_pHyprRenderer->m_renderData.transformDamage = false;
  g_pHyprRenderer->m_renderData.renderModif     = Render::SRenderModifData{};
  m_backdropScratchFB->bind();
  g_pHyprRenderer->m_renderData.currentFB = m_backdropScratchFB;
  g_pHyprOpenGL->setViewport(0, 0, W, H);
  Hyprutils::Utils::CScopeGuard guard([&] {
    g_pHyprRenderer->m_renderData.transformDamage = oldTransformDmg;
    g_pHyprRenderer->m_renderData.renderModif     = oldModif;
    g_pHyprRenderer->m_renderData.currentFB = oldFB;
    if (oldFB)
      oldFB->bind();
    g_pHyprOpenGL->setViewport(oldVp[0], oldVp[1], oldVp[2], oldVp[3]);
  });

  // Always filled: the scratch holds a previous attempt, and blending onto it would smear.
  static auto PBG = CConfigValue<Config::INTEGER>("misc:background_color");
  g_pHyprOpenGL->renderRect(CBox(0, 0, W, H), argb(*PBG, 1.0), {});

  if (m->m_background && m->m_background->ok()) {
    const double MONRATIO = static_cast<double>(W) / H;
    const double WPRATIO = m->m_background->m_size.x / m->m_background->m_size.y;
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
          pxb(CBox{pos.x - m->m_position.x, pos.y - m->m_position.y, sz.x, sz.y}, s),
          {.a = a});
      drew++;
    }
  }

  // One pixel decides. Non-black -> adopt immediately; black -> keep what we had.
  unsigned char q[4] = {0, 0, 0, 0};
  glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
  // A dark frame of a video wallpaper IS the wallpaper, so for a live source the black check
  // is skipped -- it exists to catch a texture that is not ready, which is a different thing.
  const bool haveContent = drew || m->m_background;
  const bool good = haveContent && (m_srcAnimated || !(q[0] < 8 && q[1] < 8 && q[2] < 8));
  if (good) {
    std::swap(m_backdropSrcFB, m_backdropScratchFB);
    m_backdropDrawn = true;
    m_backdropSrcTex = tex;
    // Content changed while the FB identity did not, so the blur cache key matches() compares
    // cannot see it. Without this the blur stays cached from the old wallpaper.
    m_blur.valid = false;
  }
  return m_backdropDrawn ? m_backdropSrcFB->getTexture() : nullptr;
}

// Runs on the parked-open frames. Builds the backdrop composite into a PRIVATE framebuffer and
// promotes the open the moment a non-black one exists -- the overview therefore never becomes
// visible on a frame that has nothing behind it. Two frames is the whole budget (see
// m_pendingTicks): after that it opens anyway, because a late overview beats a stuck one, and
// the frame it gives up is the single black one this whole arrangement exists to avoid.
void Overview::primeBackdrop() {
  const auto m = m_monitor.lock();
  const auto promote = [this] {
    m_pendingOpen = false;
    m_active      = true;
    m_opening     = true;
    hideLayers(); // deferred from open(): fades only once there is something drawn over it
  };

  if (!m || !g_pHyprRenderer || !g_pHyprOpenGL) {
    promote();
    damage();
    return;
  }
  const int W = m->m_pixelSize.x;
  const int H = m->m_pixelSize.y;

  const auto oldProj = g_pHyprRenderer->m_renderData.projectionType;
  const auto oldFbSz = g_pHyprRenderer->m_renderData.fbSize;
  g_pHyprRenderer->m_renderData.fbSize = Vector2D(W, H);
  g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
  const auto comp = renderBackdropSource(W, H);
  g_pHyprRenderer->m_renderData.fbSize = oldFbSz;
  g_pHyprRenderer->setProjectionType(oldProj);

  ++m_pendingTicks;
  const bool ready = comp && comp->ok();
  if (!ready && m_pendingTicks < 2) {
    damage(); // the wallpaper texture is still being created: spend one more frame on it
    return;
  }

  debug::dbg("prime: promote ticks=" + std::to_string(m_pendingTicks) +
             " backdrop=" + std::to_string(ready ? 1 : 0));
  promote();
  damage();
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
  // OPAQUE on entry, fading on exit. Opaque is what keeps the first frames off the black
  // compositor buffer: the entry base exists precisely because at low k the blend target is
  // currentFB and anything translucent lets the black frame straight back in (measured: the
  // black frame returned the moment this was made to fade both ways). On the way out the
  // opposite holds -- pinned at 1.0 the base outlived the blur and the frame it stopped
  // being painted was a visible step, measured on an empty workspace.
  const float baseA = m_opening ? 1.0F : static_cast<float>(blurAlpha());
  bool baseIsBlur = false;
  // Also while a windowed exit holds: the opaque base is what the blur sits on, and the
  // hold is exactly the moment the real desktop would otherwise show through under the
  // band. Measured without it: the band pixel stepped 171,175,148 -> 124,129,112 on the
  // frame the exit started holding, and stayed there.
  if (baseA > 0.0F) {
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
      // glReadPixels obeys the scissor: outside the box it leaves the destination alone, so
      // an unread pixel keeps the previous q and reads as black. With scissor on this probe
      // can report a perfectly good cache black.
      const bool scissorOn = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
      glDisable(GL_SCISSOR_TEST);
      for (const auto &spot : spots) {
        g_pHyprOpenGL->renderTexture(cachedTex, spot, {.a = 1.0F});
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
        if (!(q[0] < 8 && q[1] < 8 && q[2] < 8))
          lit++;
      }
      if (scissorOn)
        glEnable(GL_SCISSOR_TEST);
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
    if (cachedUsable) {
      baseIsBlur = true;
      g_pHyprOpenGL->renderTexture(cachedTex, fullPx, {.a = baseA});
    } else {
      const auto oldProj = g_pHyprRenderer->m_renderData.projectionType;
      const auto oldFbSz = g_pHyprRenderer->m_renderData.fbSize;
      g_pHyprRenderer->m_renderData.fbSize = Vector2D(W, H);
      g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
      const auto comp = renderBackdropSource(W, H);
      g_pHyprRenderer->m_renderData.fbSize = oldFbSz;
      g_pHyprRenderer->setProjectionType(oldProj);

      if (comp && comp->ok()) {
        g_pHyprOpenGL->renderTexture(comp, fullPx, {.a = baseA});
} else if (m_blur.fb && m_blur.fb->isAllocated()) {
        // No source this frame. The stale blur FB still holds the previous, correct
        // backdrop — an outdated frame beats a black one, and m_blur.valid only gates
        // recomputation, not the contents.
        // The probe above already sampled this exact texture. If it came back black it is
        // black, and painting it at baseA==1 buries the monitor framebuffer under a black
        // rectangle -- the entry blackout. Leave the base unpainted instead: currentFB then
        // keeps whatever Hyprland just rendered for this frame.
        const auto stale = m_blur.fb->getTexture();
        if (stale && stale->ok() && cachedUsable) {
          baseIsBlur = true;
          g_pHyprOpenGL->renderTexture(stale, fullPx, {.a = baseA});
        }
      }
      // Nothing painted this frame: measured once per open, the wallpaper layer
      // texture is mid-recreation and the composite is rejected as black. A fill
      // here does not help -- backdrop_color is 0x73070a10, which at any alpha is
      // (7,10,16), a black frame with a tint. Leaving it alone keeps whatever the
      // buffer already holds.
    }
  }


  if (!blurEnabled()) {
    const auto col = argb(baseCol, dimK);
    if (col.a <= 0.0)
      return;
    g_pHyprOpenGL->renderRect(fullPx, col, {});
    return;
  }

  SP<Render::ITexture> src;
  {
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
  if (!m_blur.matches(srcId, cPasses, cSize, cRes, cStrength))
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
