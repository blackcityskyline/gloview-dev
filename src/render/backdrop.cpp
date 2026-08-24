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

// ---- backdrop: source resolution, capture, cached blur, dim -----------------
//
// The backdrop is the ONE thing in the overlay that must never read currentFB
// as a source: a fullscreen window on the active workspace renders through
// Hyprland's "solitary client" fast path (renderMonitor calls renderWindow
// DIRECTLY, bypassing shouldRenderWindow entirely), and a workspace that
// never re-renders leaves currentFB holding a stale pre-overview frame. The
// source is therefore resolved from first principles every frame:
//   1. a fullscreen mpv on the displayed workspace (fullscreen_background),
//      live — re-blurred every frame, never cached;
//   2. the monitor's own wallpaper texture;
//   3. a private FBO with every BACKGROUND/BOTTOM layer-shell surface
//      (wallpaper engines) captured ONCE per session — a re-draw could pick
//      up transient window content captured by engines that screen-capture
//      the composited desktop.
// Source IDENTITY is checked EVERY frame (outside any dirty-guard): that is
// what carries the cache across plain workspace switches (same wallpaper ⇒
// no re-blur, no brightness shift) while invalidating on genuine changes.
// The cached blur is PURE — no dim, no animation factor; the dim rect and
// the fade live at the draw site.

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
        live = true; // a playing video: re-blur every frame, don't cache
        return surf->m_current.texture;
      }
    }
  }
  if (m->m_background && m->m_background->ok())
    return m->m_background;
  return nullptr;
}

SP<Render::ITexture> Overview::renderBackdropSource(int W, int H) const {
  const auto m = m_monitor.lock();
  if (!m || !g_pHyprOpenGL || !g_pHyprRenderer)
    return nullptr;

  // (Re)allocate the source FBO at the monitor's pixel size.
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

  // Once drawn for this overview session, just return the cached FBO texture
  // (see the file comment: a re-draw could capture window content).
  if (m_backdropDrawn && !needRedraw)
    return m_backdropSrcFB->getTexture();

  // Bind the FBO as both the GL target AND the renderer's currentFB (the
  // immediate-mode renderRect/renderTexture draw into currentFB), then
  // restore both plus the GL viewport on exit.
  const auto oldFB = g_pHyprRenderer->m_renderData.currentFB;
  GLint oldVp[4];
  glGetIntegerv(GL_VIEWPORT, oldVp);
  m_backdropSrcFB->bind();
  g_pHyprRenderer->m_renderData.currentFB = m_backdropSrcFB;
  g_pHyprOpenGL->setViewport(0, 0, W, H);
  Hyprutils::Utils::CScopeGuard guard([&] {
    g_pHyprRenderer->m_renderData.currentFB = oldFB;
    if (oldFB)
      oldFB->bind();
    g_pHyprOpenGL->setViewport(oldVp[0], oldVp[1], oldVp[2], oldVp[3]);
  });

  // 1. Solid background color under everything.
  static auto PBG = CConfigValue<Config::INTEGER>("misc:background_color");
  g_pHyprOpenGL->renderRect(CBox(0, 0, W, H), argb(*PBG, 1.0), {});

  // 2. Hyprland's own wallpaper texture, cover-fit like renderBackground.
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

  // 3. Every wallpaper engine's background/bottom layer-shell surface
  // (noctalia, swaybg, swww, hyprpaper, ...).
  const double s = m->m_scale;
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
    }
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

  // Crossfade factor. Deliberately just `e` — blur_strength keeps meaning
  // "filter radius", never a blend amount.
  //
  // There is deliberately NO snapshot of the pre-open FRAME under this fade:
  // a frozen copy containing windows ghosted full-size window copies under
  // the already-departing preview tiles. Window-content continuity needs no
  // snapshot anyway: frame 1 of the animation draws each preview at its
  // natural box — pixel-identical to the just-hidden real window.
  //
  // What CAN flicker is the desktop UNDER the fade: at low k the blend target
  // is whatever currentFB holds, and a transiently missing/dark wallpaper
  // shows through as a dark blink. So during the ENTRY fade an OPAQUE BASE
  // built purely from the backdrop SOURCES is drawn under everything —
  // window-free, hence ghost-free, pixel-defined from frame 0. On EXIT no
  // base: blur decays over live currentFB straight into the real desktop the
  // previews are landing on.
  const float k = static_cast<float>(e);
  debug::dbg(std::string("backdrop e=") + std::to_string(e).substr(0, 5) +
      " opening=" + std::to_string(m_opening) +
      " k=" + std::to_string(k).substr(0, 5));

  if (!blurEnabled()) {
    // ---- No-blur backdrop ----
    const auto col = argb(baseCol, e);
    if (col.a <= 0.0)
      return;
    g_pHyprOpenGL->renderRect(fullPx, col, {});
    return;
  }

  bool liveSrc = false;
  auto src = backdropSource(liveSrc);
  if (!src) {
    // Wallpaper layers → private FBO; its immediate draws want pixel coords,
    // i.e. export projection + fbSize around the call.
    const auto oldProj = g_pHyprRenderer->m_renderData.projectionType;
    const auto oldFbSz = g_pHyprRenderer->m_renderData.fbSize;
    g_pHyprRenderer->m_renderData.fbSize = Vector2D(W, H);
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    src = renderBackdropSource(W, H);
    g_pHyprRenderer->m_renderData.fbSize = oldFbSz;
    g_pHyprRenderer->setProjectionType(oldProj);
  }

  // Nothing to draw at zero; the real desktop in currentFB shows through.
  if (k <= 0.0F)
    return;

  // ---- Cached-blur backdrop ----
  const void *srcId = src ? src.get() : nullptr;
  // Full cache key: source identity + live filter recipe (config changes must
  // apply without reopening the overview). liveSrc bypasses the key entirely:
  // a playing video must re-blur EVERY frame even if the driver hands us the
  // same buffer texture twice in a row.
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

    // The blur filter manages projection/fbSize/viewport for its intermediate
    // FBOs; hold the surrounding state so the rest of the frame is intact.
    // RPT_EXPORT + fbSize=(W,H) is also what the backdrop source render and
    // the blur itself need for pixel coords.
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
      // No source at all, or blur unavailable — never leave currentFB
      // exposed (it can hold a solitary fullscreen window): cover everything
      // with an OPAQUE background rect + dim overlay, and retry next frame.
      m_blur.valid = false;
      debug::dbg("backdrop FALLBACK: blur unavailable / no source");
      static auto PBG = CConfigValue<Config::INTEGER>("misc:background_color");
      g_pHyprOpenGL->renderRect(fullPx, argb(*PBG, 1.0), {});
      g_pHyprOpenGL->renderRect(fullPx, argb(baseCol, e), {});
      return;
    }
    m_blur.valid = true;
  }

  // Cheap path: blit the cached blurred backdrop, faded by the animation
  // curve, then the dim rect on top.
  const auto tex = m_blur.fb ? m_blur.fb->getTexture() : nullptr;
  // EXIT uses a slower tail (pow < 1 lifts small alphas): a linear decay is
  // perceptually "gone" for the last ~30% of the travel while the big tiles
  // are still shrinking home, which read as a blur gap right before landing.
  const float kk = m_opening ? k : std::pow(k, 0.45F);
  if (tex && tex->ok()) {
    g_pHyprOpenGL->renderTexture(tex, fullPx, {.a = kk});
  } else {
    m_blur.valid = false;
    // Cache unavailable — never leave the base/currentFB exposed (see the
    // blur-fail path above). Draw an opaque background rect instead.
    static auto PBG2 = CConfigValue<Config::INTEGER>("misc:background_color");
    g_pHyprOpenGL->renderRect(fullPx, argb(*PBG2, 1.0), {});
  }
  // Dim rides ON TOP of the blur (not baked into the cache): its alpha
  // follows the same curve, and backdrop_color changes apply live without
  // invalidating the cached blur.
  const auto dimCol = argb(baseCol, e);
  if (dimCol.a > 0.0)
    g_pHyprOpenGL->renderRect(fullPx, dimCol, {});
}

} // namespace gloview
