#include "overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

double nowMs(const std::chrono::steady_clock::time_point &from) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - from)
      .count();
}

double easeOutCubic(double t) {
  t = std::clamp(t, 0.0, 1.0);
  const double inv = 1.0 - t;
  return 1.0 - inv * inv * inv;
}

// Decelerate with a gentle overshoot — tiles "pop" as they settle into their
// slot.
double easeOutBack(double t) {
  t = std::clamp(t, 0.0, 1.0);
  const double c1 = 1.70158 * 0.6; // softened overshoot
  const double c3 = c1 + 1.0;
  const double inv = t - 1.0;
  return 1.0 + c3 * inv * inv * inv + c1 * inv * inv;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

CBox box(const LRect &r) { return CBox{r.x, r.y, r.w, r.h}; }

// Hyprland's immediate-mode renderRect/renderTexture/renderRoundedShadow feed
// the box STRAIGHT to projectBoxToTarget, which expects transformed
// monitor-PIXEL coordinates and applies NO monitor scale itself (verified
// against Renderer.cpp: clipBox/scaledWindowBox are pre-.scale(m_scale)'d
// before applyToBox). All gloview chrome is authored in monitor-LOGICAL pixels,
// so it MUST be pre-scaled by mon->m_scale before drawing — otherwise on any
// monitor with scale != 1 (HiDPI / fractional like 1.2) the whole chrome
// renders at 1/scale size and top-left-biased, while the live window surfaces
// (renderWindowLive, which converts to pixels itself) land correctly → the
// overview looks "distorted". Chrome-only; surfaces are already pixel-space.
// Round radii / blur ranges scale too so corners/shadows keep their proportion.
CBox pxb(const CBox &b, double s) {
  return CBox{b.x * s, b.y * s, b.w * s, b.h * s};
}
CBox pxb(const LRect &r, double s) {
  return CBox{r.x * s, r.y * s, r.w * s, r.h * s};
}
int pxr(double round, double s) { return static_cast<int>(round * s); }

// The unified preview_round (task #6) is authored against full-size grid tiles;
// strip card previews are much smaller, so a raw radius that's fine on a grid
// tile can exceed a tiny card slot's half-size and look broken. Clamp per call
// site to whatever the slot allows.
int clampRound(int round, double w, double h) {
  return static_cast<int>(
      std::clamp(static_cast<double>(round), 0.0, std::min(w, h) * 0.5));
}

// Real, current per-window opacity — mirrors IHyprRenderer::renderWindow's
// renderdata.alpha / renderdata.fadeAlpha (Renderer.cpp): the ACTIVE/INACTIVE
// opacity windowrule (or 1 if the "opaque" rule forces it), times the
// FADE/FULLSCREEN/LAYOUT fade animations. Deliberately excludes the
// workspace-switch-only components (WINDOW_ALPHA_MOVE_TO/FROM_WORKSPACE, and
// the workspace's own m_alpha) — those are real-desktop transition artifacts
// that would otherwise make an off-workspace window's PREVIEW flicker/fade in
// lockstep with a switch animation it isn't actually part of. Without this
// every preview rendered fully opaque regardless of the window's real
// transparency (an `opacity` windowrule, a window fading in/out, …), which is
// why a transparent terminal looked solid on the strip/grid while looking
// transparent for real.
float windowRealAlpha(const PHLWINDOW &w) {
  if (!w)
    return 1.0F;
  float active = w->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
  if (w->m_ruleApplicator && w->m_ruleApplicator->opaque().valueOrDefault())
    active = 1.0F;
  const float fade = w->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) *
                     w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) *
                     w->alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT);
  return std::clamp(active * fade, 0.0F, 1.0F);
}

// Mirrors IHyprRenderer::shouldBlur(PHLWINDOW) (Renderer.cpp; private there, so
// re-derived here from the same public bits it reads) — blur-behind is a
// per-window opt-out (noblur/RGBX windowrules, or a surface Hyprland already
// knows is fully opaque) gated by the global decoration:blur:enabled toggle,
// exactly like any window on the real desktop. Without this every preview had
// blur hard-disabled, so a transparent window showed hard, unblurred edges in
// the overview instead of the frosted look it actually has.
bool windowShouldBlur(const PHLWINDOW &w) {
  if (!w || !w->m_ruleApplicator)
    return false;
  static auto PBLUR = CConfigValue<Config::INTEGER>("decoration:blur:enabled");
  const bool dontBlur = w->m_ruleApplicator->noBlur().valueOrDefault() ||
                        w->m_ruleApplicator->RGBX().valueOrDefault() ||
                        w->opaque();
  return *PBLUR != 0 && !dontBlur;
}

} // namespace

// Render a window's LIVE surface tree scaled into `destPx`, clipped to `clipPx`
// (both monitor PIXEL coords) via real CSurfacePassElements. No crop rect to
// drift, so immune to snapshots' stale/mis-cropped tiles; works on hidden
// workspaces. `roundPx` (destination-pixel-space corner radius) and
// `roundingPower` let the caller match whatever rounding the surrounding chrome
// (border ring / backing) uses. External linkage (regular namespace-scope, not
// anonymous): shared with overview_tiles_render.cpp
// (renderMainWindows/renderDragWindow), which forward-declares this exact
// signature rather than duplicating the ~70-line body.
void renderWindowLive(const PHLWINDOW &w, const PHLMONITOR &mon,
                      const CBox &destPx, const CBox &clipPx, float alpha,
                      const Time::steady_tp &when, int roundPx = 0,
                      float roundingPower = 2.0F) {
  if (!w || !mon || !w->m_isMapped || !w->wlSurface() ||
      !w->wlSurface()->resource())
    return;
  if (!(destPx.w > 0 && destPx.h > 0))
    return;

  // When reported size > committed buffer (CWLSurface::small(): X11 size
  // hints/mid-resize), getTexBox CENTERS it at real size, leaving an uncovered
  // margin. m_fillIgnoreSmall stretches to fill; Hyprland never sets it, so
  // restoreFill() resets on teardown.
  w->wlSurface()->m_fillIgnoreSmall = true;

  // SETTLED goal() geometry, not mid-animation value(): destPx is sized from
  // goal(), so scaling by value() mid-resize fills only part of the box → black
  // side strips. Position cancels in the translate remap below, so only size
  // matters.
  const auto pos = w->m_realPosition->goal();
  const auto size = w->m_realSize->goal();
  const float logicalW = std::max((float)size.x, 5.F);
  const float logicalH = std::max((float)size.y, 5.F);
  // Over-cover the slot (fill BOTH axes via max + ~1.5px pad), don't just fit
  // it: a fit-exact scale rounds the surface edge 1-3px inside the box and the
  // opaque backing peeks through as thin dark seams (worst mid open-glide,
  // destPx fractional every frame). TL stays anchored (translate cancels
  // scaleMod); clipPx trims the overflow.
  const float pad = 1.5F;
  const float sW = (static_cast<float>(destPx.w) + pad) /
                   std::max(logicalW * mon->m_scale, 5.F);
  const float sH = (static_cast<float>(destPx.h) + pad) /
                   std::max(logicalH * mon->m_scale, 5.F);
  const float scaleMod = std::max(sW, sH);
  if (!(scaleMod > 0.F))
    return;

  const Vector2D logicalTL = pos + w->m_floatingOffset;
  const Vector2D scaledTL = (logicalTL - mon->m_position) * mon->m_scale;
  const Vector2D translate = destPx.pos() / scaleMod - scaledTL;

  Render::SRenderModifData modif;
  modif.modifs.push_back(
      {Render::SRenderModifData::eRenderModifType::RMOD_TYPE_TRANSLATE,
       std::any(translate)});
  modif.modifs.push_back(
      {Render::SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE,
       std::any(scaleMod)});
  modif.enabled = true;

  g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(
      CRendererHintsPassElement::SData{.renderModif = modif}));
  Hyprutils::Utils::CScopeGuard reset([] {
    g_pHyprRenderer->m_renderPass.add(
        makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{
            .renderModif = Render::SRenderModifData{}}));
  });

  g_pHyprRenderer->damageWindow(w);

  CSurfacePassElement::SRenderData data{};
  data.pMonitor = mon;
  data.when = when;
  data.pos = logicalTL;
  data.w = std::max(size.x, 5.0);
  data.h = std::max(size.y, 5.0);
  data.surface = w->wlSurface()->resource();
  // NOT tied to the window's real fullscreen state: on the real desktop a
  // fullscreen window's edge-to-edge presentation makes rounded corners look
  // wrong, hence Hyprland's own renderer squares them off — but here this is
  // always a shrunk-down PREVIEW tile or strip-card thumbnail, where that
  // distinction is meaningless and just produced a visibly square-cornered
  // preview specifically for whichever workspace happened to have a fullscreen
  // video/window on it, inconsistent with every other card/tile.
  data.dontRound = false;
  // Real per-window opacity (windowrule opacity, fade-in/out, …) times the
  // caller's own overview-chrome fade (`alpha`, e.g. the open/close eased
  // progress) — both factors multiply in CSurfacePassElement's draw, so either
  // field can carry either one; splitting them this way keeps `alpha` meaning
  // exactly what every call site already passes it as.
  data.fadeAlpha = windowRealAlpha(w);
  data.alpha = std::clamp(alpha, 0.F, 1.F);
  data.decorate = false;
  data.rounding = roundPx;
  data.roundingPower = roundingPower;
  // Real blur-behind eligibility (decoration:blur:enabled + the window's own
  // noblur/RGBX/ opaque state) instead of a hardcoded `false` — a transparent
  // preview otherwise showed hard edges instead of the frosted look the window
  // actually has on the real desktop.
  data.blur = windowShouldBlur(w);
  data.pWindow = w;
  data.clipBox = clipPx;
  data.squishOversized = true;
  data.surfaceCounter = 0;

  w->wlSurface()->resource()->breadthfirst(
      [&data, &w](SP<CWLSurfaceResource> s, const Vector2D &offset, void *) {
        if (!s || !s->m_current.texture || s->m_current.size.x < 1 ||
            s->m_current.size.y < 1)
          return;
        data.localPos = offset;
        data.texture = s->m_current.texture;
        data.surface = s;
        data.mainSurface = s == w->wlSurface()->resource();
        g_pHyprRenderer->m_renderPass.add(
            makeUnique<CSurfacePassElement>(data));
        data.surfaceCounter++;
      },
      nullptr);
}

namespace {
CHyprColor argb(Hyprlang::INT raw, double alphaMul = 1.0) {
  const auto a = static_cast<double>((raw >> 24) & 0xFF) / 255.0;
  const auto r = static_cast<double>((raw >> 16) & 0xFF) / 255.0;
  const auto g = static_cast<double>((raw >> 8) & 0xFF) / 255.0;
  const auto b = static_cast<double>(raw & 0xFF) / 255.0;
  return CHyprColor(r, g, b, a * std::clamp(alphaMul, 0.0, 1.0));
}
// Immediate-mode overlay chrome split into phases so the LIVE window surfaces
// (queued CSurfacePassElements, not immediate GL) layer between the chrome:
// backdrop+backings (Back) → main surfaces → strip chrome (Mid) → strip
// surfaces → dragged tile chrome (DragBack) → dragged surface → cursor (Front).
class COverlayPass final : public IPassElement {
public:
  // Buttons/StripButtons draw AFTER their surfaces are queued
  // (Back→surfaces→Buttons, Mid→surfaces→StripButtons): a close button sits
  // INSIDE the tile/card bounds, so if it drew in the same phase as the chrome
  // (before the live surface), any opaque window content painted the button
  // right over — it only ever showed through on windows with a transparent
  // corner (e.g. a translucent terminal), which is why it looked like it only
  // "worked" for some apps and not others. The hover/select RING and shadow
  // don't have this problem (they're drawn OUTSIDE the tile bounds, never
  // covered), so they can stay in Back/Mid.
  enum class Phase { Back, Buttons, Mid, StripButtons, DragBack, Front };
  COverlayPass(Overview *o, Phase phase) : m_owner(o), m_phase(phase) {}

  std::vector<UP<IPassElement>> draw() override {
    if (!m_owner)
      return {};
    switch (m_phase) {
    case Phase::Back:
      m_owner->renderBackdrop();
      m_owner->renderPreviews(); // main tile chrome (shadow/ring/backing);
                                 // surfaces queued right after
      break;
    case Phase::Buttons:
      m_owner->renderTileButtons();
      break; // per-window "✕", on top of the surfaces
    case Phase::Mid:
      m_owner->renderStrip();
      break; // strip chrome; surfaces queued after
    case Phase::StripButtons:
      m_owner->renderStripButtons();
      break; // per-card "✕" + drop-quadrant hint, on top
    case Phase::DragBack:
      m_owner->renderDragTile();
      break; // dragged tile chrome; surface queued after
    case Phase::Front:
      m_owner->renderCursorOnTop();
      break;
    }
    return {};
  }

  // Back (backdrop) and Mid (strip band) draw blurred rects. Hyprland refreshes
  // the live-blur framebuffer only from elements reporting true; false → stale
  // blur residue.
  bool needsLiveBlur() override {
    return (m_phase == Phase::Back || m_phase == Phase::Mid) && m_owner &&
           m_owner->blurEnabled();
  }
  bool needsPrecomputeBlur() override { return false; }
  // Occlusion culling must be off while the overview is up. The queued preview
  // surfaces (CSurfacePassElement) report boundingBox/opaqueRegion at the
  // window's REAL footprint — they can't see our translate+scale render-modif —
  // so once the open animation settles (alpha hits 1, opaqueRegion turns
  // non-empty) each preview "occludes" its real, often monitor-filling rect and
  // CRenderPass::simplify() empties the damage of every element below it:
  // backdrop, strip chrome, other previews, the background layer. With blur ≠ 0
  // needsLiveBlur() masked this (the live-blur region neutralizes all opaque
  // subtraction in simplify()); with blur = 0 the overview drew for the open
  // animation, then collapsed to bare wallpaper/stale frames. Whenever a frame
  // DOES render, the whole overlay still has to be repainted top to bottom
  // regardless (it's one visual unit), so skipping simplify() doesn't add extra
  // frames — it only affects work already happening within a frame that was
  // going to render anyway.
  bool disableSimplification() override { return true; }
  bool undiscardable() override { return true; }
  const char *passName() override { return "GloviewOverlayPass"; }
  ePassElementType type() override { return EK_CUSTOM; }
  std::optional<CBox> boundingBox() override {
    const auto m = m_owner ? m_owner->monitor() : nullptr;
    if (!m)
      return std::nullopt;
    return CBox{{}, m->m_size};
  }

private:
  Overview *m_owner = nullptr;
  Phase m_phase = Phase::Back;
};

} // namespace

// ---- animation --------------------------------------------------------------

double Overview::eased() const { return easeOutCubic(m_progress); }

double Overview::tileBaseProgress() const {
  // During a drop reflow the tiles glide on their own timer while the chrome
  // (m_progress) stays pinned at 1; everywhere else they ride m_progress.
  if (m_reflowing) {
    const double dur = std::max(1, cfgInt("plugin:gloview:duration", 360));
    return std::clamp(nowMs(m_reflowStart) / dur, 0.0, 1.0);
  }
  return m_progress;
}

double Overview::tileProgress(int i) const {
  const double base = tileBaseProgress();
  const int n = static_cast<int>(m_tiles.size());
  if (n <= 1)
    return base;
  const double spread = std::min(0.35, 0.05 * n); // total cascade window
  const double start = spread * (static_cast<double>(i) / (n - 1));
  const double span = std::max(0.001, 1.0 - spread);
  return std::clamp((base - start) / span, 0.0, 1.0);
}

LRect Overview::currentBox(const Tile &t, int i) const {
  const double e = easeOutBack(tileProgress(i));
  const auto &a = t.natural;
  const auto &b = t.target;
  return LRect{lerp(a.x, b.x, e), lerp(a.y, b.y, e), lerp(a.w, b.w, e),
               lerp(a.h, b.h, e)};
}

void Overview::updateAnimation() {
  const double dur = std::max(1, cfgInt("plugin:gloview:duration", 360));
  const double t = std::clamp(nowMs(m_animStart) / dur, 0.0, 1.0);
  m_progress = m_opening ? t : 1.0 - t;
  if (m_reflowing && nowMs(m_reflowStart) >= dur)
    m_reflowing = false;
  if (m_newCardAnim &&
      nowMs(m_newCardStart) >=
          std::max(120, cfgInt("plugin:gloview:duration", 360))) {
    m_newCardAnim = false;
    m_newCardId = 0;
  }
  // Close complete: DON'T deactivate here — flipping m_active off mid-frame
  // would make renderStage skip this frame's overlay → one transparent frame
  // (real windows already suppressed). Pin progress to 0, let renderStage draw
  // the final opaque-preview frame, then deactivate once the pass is built
  // (m_pendingDeactivate).
  if (!m_opening && t >= 1.0) {
    m_progress = 0.0;
    m_pendingDeactivate = true;
  }
}

// ---- render -----------------------------------------------------------------

void Overview::renderStage(eRenderStage stage) {
  if (!m_active)
    return;
  // captureSnapshots drives a nested render pass that re-emits render-stage
  // events; without this guard we'd re-add the overlay pass mid-snapshot →
  // reentrant render → SEGV.
  if (m_capturing)
    return;
  const auto rm = g_pHyprRenderer->m_renderData.pMonitor.lock();
  const auto m = m_monitor.lock();
  // RENDER_LAST_MOMENT is after the top/overlay layers (bars), so the overview
  // paints over them instead of the bar bleeding on top.
  if (!rm || !m || rm != m)
    return;

  if (stage != RENDER_LAST_MOMENT)
    return;

  updateAnimation();
  if (!m_active)
    return;

  updateHover(); // keep hover fresh even when the pointer is warped, not moved
  syncTiles(); // window opened/closed/moved on this workspace → reflow the grid

  // Live workspace changed underneath us — a passthrough keybind we don't
  // intercept ourselves (`workspace N`, a waybar/widget click, anything outside
  // gloview). Previously this was ONLY ever handled when exit_on_switch was on;
  // with it off (the default) nothing reacted at all, so the overview just sat
  // frozen on stale state — the real desktop visibly changed underneath (e.g.
  // in a shell widget) while gloview kept showing the workspace it had when it
  // opened, with clicks/keys inside it fully desynced from what's actually on
  // screen.
  if (m_opening && m->m_activeWorkspace != m_liveWsAtOpen.lock()) {
    m_liveWsAtOpen = m->m_activeWorkspace;
    if (cfgInt("plugin:gloview:exit_on_switch", 0) != 0) {
      m_workspace = m->m_activeWorkspace; // accept the external switch so
                                          // deactivate() doesn't revert it
      close();
    } else if (!showAllWorkspaces()) {
      // Follow along instead of ignoring it: treat it like the user clicked
      // that workspace's card, so the grid/strip stay in sync with what's
      // really on screen.
      m_workspace = m->m_activeWorkspace;
      buildTiles();
      buildStrip();
      layoutTiles();
      if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
        m_selected = m_tiles.empty() ? -1 : 0;
      damage();
    } else {
      // All-workspaces (expo): the grid already spans every workspace, so a
      // live switch doesn't change WHICH windows are shown — just refresh which
      // strip card is highlighted "active".
      buildStrip();
      damage();
    }
  }

  // Layer order: backdrop + main tile chrome → main surfaces → tile buttons →
  // strip chrome → strip surfaces → strip buttons → drag chrome → drag surface
  // → cursor. The immediate-mode chrome is split across COverlayPass phases so
  // the queued surfaces slot between them (and the "✕" buttons draw AFTER their
  // surface, see the Phase comment).
  auto &pass = g_pHyprRenderer->m_renderPass;
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Back));
  renderMainWindows();
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Buttons));
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Mid));
  renderStripWindows();
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::StripButtons));
  const bool draggingTile = m_dragging && m_pressTile >= 0 &&
                            m_pressTile < static_cast<int>(m_tiles.size());
  const bool draggingStripWin =
      m_dragging && m_pressStripItem >= 0 && !m_dragStripWin.expired();
  const bool dragging = draggingTile || draggingStripWin;
  if (dragging) {
    pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::DragBack));
    renderDragWindow();
  }
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Front));

  renderAboveLayers(); // opted-in layer surfaces (e.g. the live-input HUD) sit
                       // on top of the overview

  // Final close frame: the overlay (opaque previews at natural positions) is
  // now queued, covering the windows shouldRenderWindow suppressed earlier this
  // frame. Flip off NOW, after the pass is built, so the NEXT frame renders the
  // real windows — pixel-perfect handoff, no transparent gap. The queued
  // surfaces already captured their data; the deferred chrome callbacks no-op
  // at progress 0. deactivate() damages the next frame.
  if (m_pendingDeactivate) {
    m_pendingDeactivate = false;
    deactivate();
    return;
  }

  // Keep repainting every frame only while something is actually animating
  // (open/close glide, post-drop reflow, "+" pop-in, an in-progress drag) or a
  // recapture tick is pending. Once things settle, stop forcing a full-monitor
  // damage every single frame — event-driven damage (mouse move, click, key
  // press, hover/selection change; all of those paths already call damage()
  // themselves) repaints on any real change just fine. This used to
  // unconditionally damage() here, which pinned the compositor at a continuous
  // full-monitor redraw (plus live-blur recompute over the whole backdrop/strip
  // every single frame) for the ENTIRE time the overview was open, even sitting
  // idle — this was the dominant cause of the high CPU/GPU load while the
  // overview is up. The trade-off: if something OUTSIDE gloview damages only a
  // small sub-region while we're idle (e.g. a background client repainting),
  // that partial redraw could in principle show a blur edge seam at its border
  // instead of always being masked by our own full-frame repaint; in practice
  // this is far rarer than the constant-redraw cost it replaces.
  //
  // m_altTabbing is deliberately NOT part of this: it stays true for the ENTIRE
  // alt-tab session (from the moment it opens until it's committed or
  // cancelled), not just while a step animation is in flight, so including it
  // here forced the exact same continuous full-monitor redraw + live-blur
  // recompute this comment describes fixing — just scoped to alt-tab instead of
  // every session, which is exactly the "GPU pegged at ~50% for as long as
  // alt-tab/expo stays open, single-workspace mode only spikes briefly" bug.
  // Every actual change during a session (a tab step, a hover/selection change,
  // a click) already calls damage() itself (stepAltTab, moveSelection,
  // updateHover, …), so nothing needs a per-frame poke on top of that.
  const bool animating =
      m_reflowing || m_newCardAnim || m_dragging || m_recaptureLeft > 0 ||
      (m_opening && m_progress < 1.0) || (!m_opening && m_progress > 0.0);
  if (animating)
    damage();
}

void Overview::renderBackdrop() const {
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double e = eased();
  const auto col =
      argb(cfgColor("plugin:gloview:backdrop_color", 0x73070a10), e);
  if (col.a <= 0.0)
    return;
  const double s = m->m_scale;
  g_pHyprOpenGL->renderRect(
      pxb(CBox(0, 0, m->m_size.x, m->m_size.y), s), col,
      {.blur = blurEnabled(), .blurA = static_cast<float>(e) * blurStrength()});
}

void Overview::renderStrip() const {
  const auto m = m_monitor.lock();
  if (!m || m_strip.empty())
    return;
  const double e = eased();
  if (e <= 0.01)
    return;
  const double s =
      m->m_scale; // logical→pixel; Hyprland's renderRect wants pixel coords
  const int previewRound = cfgInt("plugin:gloview:preview_round", 12);
  const float roundPow = cfgFloat("plugin:gloview:preview_round_power", 2.0F);

  // translucent band behind the cards (kept faint per request)
  const auto bandCol =
      argb(cfgColor("plugin:gloview:strip_band_color", 0x24ffffff), e);
  const bool blur = blurEnabled();
  const LRect bandR = stripBand();
  const Vector2D slide =
      stripSlide(e); // slide the whole strip in from its edge
  const Vector2D scroll = stripScroll(); // scroll the card group along the band
  g_pHyprOpenGL->renderRect(
      pxb(CBox(bandR.x + slide.x, bandR.y + slide.y, bandR.w, bandR.h), s),
      bandCol, {.blur = blur, .blurA = static_cast<float>(e) * blurStrength()});

  const int cardRound = cfgInt("plugin:gloview:strip_card_round", 10);
  const auto cardBg =
      argb(cfgColor("plugin:gloview:strip_card_color", 0x3a0e131c), e);
  const auto activeBg =
      argb(cfgColor("plugin:gloview:strip_active_color", 0x4d1c2c44), e);
  const auto activeLine =
      argb(cfgColor("plugin:gloview:strip_active_border", 0xf0ffffff), e);
  const auto hoverLine =
      argb(cfgColor("plugin:gloview:strip_hover_border", 0x80ffffff), e);
  const auto plusCol =
      argb(cfgColor("plugin:gloview:strip_plus_color", 0xd0eef4ff), e);
  // Expo indicator: when all-workspaces is active, the "All" card (if present)
  // lights up active-style; otherwise outline every real card for feedback. The
  // live workspace keeps its activeBg fill either way.
  const bool allWs = showAllWorkspaces();
  const bool allCardShown = cfgInt("plugin:gloview:strip_all_card", 0) != 0;

  for (size_t i = 0; i < m_strip.size(); ++i) {
    const auto &it = m_strip[i];
    const bool hover = static_cast<int>(i) == m_hoveredStrip;
    LRect card = it.card;
    card.x += slide.x + scroll.x; // follow the strip slide-in and scroll
    card.y += slide.y + scroll.y;
    if (m_newCardAnim && it.id == m_newCardId && !it.isPlus && !it.isAll) {
      const double f = newCardScale(); // pop-in: scale up from the card center
      const double cx = card.cx(), cy = card.cy();
      card = LRect{cx - card.w * f / 2.0, cy - card.h * f / 2.0, card.w * f,
                   card.h * f};
    }
    const CBox c = box(card);

    // border frame underlay: one rounded rect grown by the line width, so the
    // card body on top leaves a clean ring (four thin strips would blob at the
    // corners).
    const bool actLike =
        it.active || (allWs && allCardShown && it.isAll); // filled + thick ring
    const bool expoRing =
        allWs && !allCardShown && !it.isPlus; // outline-all fallback
    const bool ring = actLike || expoRing;
    if (ring || hover) {
      const auto &lc = ring ? activeLine : hoverLine;
      const double t = actLike ? 2.5 : 2.0;
      g_pHyprOpenGL->renderRect(
          pxb(CBox(c.x - t, c.y - t, c.w + 2 * t, c.h + 2 * t), s), lc,
          {.round = pxr(cardRound + t, s), .roundingPower = roundPow});
    }

    g_pHyprOpenGL->renderRect(
        pxb(c, s), actLike ? activeBg : cardBg,
        {.round = pxr(cardRound, s), .roundingPower = roundPow});

    if (it.isPlus) {
      // draw a centered plus
      const double t = std::max(2.0, card.h * 0.04);
      const double L = std::min(card.w, card.h) * 0.34;
      const double cx = card.cx(), cy = card.cy();
      g_pHyprOpenGL->renderRect(pxb(CBox(cx - L / 2, cy - t / 2, L, t), s),
                                plusCol, {.round = pxr(t / 2, s)});
      g_pHyprOpenGL->renderRect(pxb(CBox(cx - t / 2, cy - L / 2, t, L), s),
                                plusCol, {.round = pxr(t / 2, s)});
    } else if (it.isAll) {
      // 2x2 grid-of-squares glyph = "all windows / every workspace"
      const double pad = std::min(card.w, card.h) * 0.26;
      const double gw = card.w - 2 * pad, gh = card.h - 2 * pad;
      const double cg =
          std::max(2.0, std::min(card.w, card.h) * 0.07); // gap between cells
      const double cw = std::max(2.0, (gw - cg) / 2.0),
                   ch = std::max(2.0, (gh - cg) / 2.0);
      const double gx = card.x + pad, gy = card.y + pad;
      for (int r = 0; r < 2; ++r)
        for (int col = 0; col < 2; ++col)
          g_pHyprOpenGL->renderRect(
              pxb(CBox(gx + col * (cw + cg), gy + r * (ch + cg), cw, ch), s),
              plusCol, {.round = pxr(2, s)});
    } else {
      // Opaque backing per window slot: the live surface (queued on top by
      // renderStripWindows) may carry transparency, so without it the
      // translucent card band over the blurred backdrop bleeds through.
      // Rounding is unified onto preview_round/preview_round_power (tasks
      // #4/#6) instead of the old, separate strip_preview_round — clamped to
      // each slot's size so a large preview_round doesn't blow out on these
      // much smaller card thumbnails.
      for (size_t j = 0; j < it.wins.size(); ++j) {
        const auto &sw = it.wins[j];
        const auto w = sw.win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
          continue;
        // inset 1px so the backing stays under the live surface and can't peek
        // as a thin dark edge line (see drawPreviewTile).
        const LRect wbL = stripWinSlotRect(it, card, j);
        const CBox wb(wbL.x + 1.0, wbL.y + 1.0, std::max(2.0, wbL.w - 2.0),
                      std::max(2.0, wbL.h - 2.0));
        const int wRound = clampRound(previewRound, wbL.w, wbL.h);
        // Grab indicator (task #6): a bright highlight around the exact slot
        // that's currently pressed, before it's lifted into a floating drag — a
        // static highlight rather than a blink/pulse so it doesn't need to
        // force continuous repainting while the mouse just sits still holding
        // the button down.
        const bool grabbed = static_cast<int>(i) == m_pressStripItem &&
                             static_cast<int>(j) == m_pressStripWin &&
                             !(m_dragging && m_pressStripItem >= 0);
        if (grabbed) {
          const double gt = 2.0;
          g_pHyprOpenGL->renderRect(
              pxb(CBox(wb.x - gt, wb.y - gt, wb.w + 2 * gt, wb.h + 2 * gt), s),
              argb(cfgColor("plugin:gloview:hover_border", 0xf0ffffff), e),
              {.round = pxr(wRound + static_cast<int>(gt), s),
               .roundingPower = roundPow});
        }
        g_pHyprOpenGL->renderRect(
            pxb(wb, s), argb(0xff14181f, e),
            {.round = pxr(wRound, s), .roundingPower = roundPow});
      }
    }

    // workspace label, centered above the card
    if (it.label && it.label->m_size.x > 0) {
      double lw = it.label->m_size.x;
      double lh = it.label->m_size.y;
      const double maxLw = card.w + 24.0;
      if (lw > maxLw) {
        const double s = maxLw / lw;
        lw *= s;
        lh *= s;
      }
      // labelH (26) is reserved above every card by buildStrip (both layouts).
      const double labelBand = 26.0;
      const double lx = card.cx() - lw / 2.0;
      const double ly = card.y - labelBand + (labelBand - lh) / 2.0;
      const float la =
          it.active ? static_cast<float>(e) : static_cast<float>(e) * 0.75F;
      g_pHyprOpenGL->renderTexture(it.label, pxb(CBox(lx, ly, lw, lh), s),
                                   {.a = la});
    }
  }
}

// Per-card "close every window on this workspace" button + the drag destination
// hint, drawn in their OWN phase AFTER renderStripWindows() has queued the
// cards' live surfaces (see the Phase::StripButtons comment on COverlayPass) —
// both sit ON TOP of the card's window previews, which used to be drawn UNDER
// them in renderStrip() and so got covered by any opaque window content.
void Overview::renderStripButtons() const {
  const auto m = m_monitor.lock();
  if (!m || m_strip.empty())
    return;
  const double e = eased();
  if (e <= 0.01)
    return;
  const double s = m->m_scale;
  const int cardRound = cfgInt("plugin:gloview:strip_card_round", 10);
  const float roundPow = cfgFloat("plugin:gloview:preview_round_power", 2.0F);
  const bool showClose = m_desktopMode || closeButtonsAlwaysOn();
  const bool dropping =
      m_dragging && (m_pressTile >= 0 || m_pressStripItem >= 0);

  for (size_t i = 0; i < m_strip.size(); ++i) {
    const auto &it = m_strip[i];
    if (it.isPlus || it.isAll)
      continue;
    const LRect card = stripCardAt(i);

    // Destination placement hint: while dragging a window (from the grid or
    // straight off another card) over THIS card, split it into quadrants by
    // cursor position and highlight the nearest one — a spatial hint of roughly
    // where the window would land in that workspace's tiling, not a guarantee
    // of the exact slot (the real tiling layout has the final say once it's
    // actually dropped there).
    if (dropping && static_cast<int>(i) == m_hoveredStrip) {
      const double qx = (m_dragX < card.cx()) ? card.x : card.cx();
      const double qy = (m_dragY < card.cy()) ? card.y : card.cy();
      const LRect quad{qx, qy, card.w / 2.0, card.h / 2.0};
      g_pHyprOpenGL->renderRect(
          pxb(quad, s), CHyprColor(1.0, 1.0, 1.0, 0.22 * e),
          {.round = pxr(cardRound / 2, s), .roundingPower = roundPow});
    }

    // "close every window on this workspace" — the visible counterpart to the
    // middle-click shortcut, shown under the same visibility rule as the
    // per-window "✕". Stays a plain circle (default roundingPower) regardless
    // of preview_round_power — a "squircle" close button would look broken at
    // non-default curve exponents.
    if (showClose) {
      const LRect br = stripCloseButtonRect(card);
      const double rad = br.w / 2.0;
      g_pHyprOpenGL->renderRect(
          pxb(box(br), s),
          argb(cfgColor("plugin:gloview:close_button_color", 0xe6e23b3b), e),
          {.round = pxr(rad, s)});
      if (m_closeGlyph && m_closeGlyph->m_size.x > 0) {
        const double gw = m_closeGlyph->m_size.x * 0.62,
                     gh = m_closeGlyph->m_size.y * 0.62;
        g_pHyprOpenGL->renderTexture(
            m_closeGlyph,
            pxb(CBox(br.cx() - gw / 2.0, br.cy() - gh / 2.0, gw, gh), s),
            {.a = static_cast<float>(e)});
      }
    }
  }
}

// Queues the LIVE surfaces for every strip card, layered above the BACK chrome
// (card backings) but under the FRONT chrome (drag tile, cursor).
void Overview::renderStripWindows() const {
  const auto m = m_monitor.lock();
  if (!m || m_strip.empty())
    return;
  const double e = eased();
  if (e <= 0.01)
    return;
  // mirror renderStrip()'s slide-in + scroll so the previews travel with their
  // cards
  const Vector2D slide = stripSlide(e);
  const Vector2D scroll = stripScroll();
  const double scale = m->m_scale;
  const auto when = Time::steadyNow();
  const int previewRound = cfgInt("plugin:gloview:preview_round", 12);
  const float roundPow = cfgFloat("plugin:gloview:preview_round_power", 2.0F);

  for (size_t i = 0; i < m_strip.size(); ++i) {
    const auto &it = m_strip[i];
    if (it.isPlus || it.isAll) // neither carries window previews
      continue;
    LRect card = it.card;
    card.x += slide.x + scroll.x;
    card.y += slide.y + scroll.y;
    for (size_t j = 0; j < it.wins.size(); ++j) {
      // being dragged as a floating preview right now → drawn separately, skip
      // here
      if (m_dragging && static_cast<int>(i) == m_pressStripItem &&
          static_cast<int>(j) == m_pressStripWin)
        continue;
      const auto w = it.wins[j].win.lock();
      if (!w || !w->m_isMapped || w->isHidden())
        continue;
      // window slot inside the card, from its tiled goal position (logical)
      const LRect slot = stripWinSlotRect(it, card, j);
      const int round = pxr(clampRound(previewRound, slot.w, slot.h), scale);
      // renderWindowLive works in monitor PIXEL coords; the card chrome
      // (renderStrip) is pre-scaled to pixels too (pxb), so surface and backing
      // coincide at any monitor scale.
      const CBox slotPx(slot.x * scale, slot.y * scale, slot.w * scale,
                        slot.h * scale);
      const CBox cardPx(card.x * scale, card.y * scale, card.w * scale,
                        card.h * scale);
      renderWindowLive(w, m, slotPx, cardPx, static_cast<float>(e), when, round,
                       roundPow);
    }
  }
}

bool Overview::isAboveLayer(const std::string &ns) const {
  if (ns.find("aboveoverview") != std::string::npos)
    return true;
  const std::string list = cfgStr("plugin:gloview:above_namespaces", "");
  size_t i = 0;
  while (i < list.size()) {
    // split on commas AND whitespace
    while (
        i < list.size() &&
        (list[i] == ',' || std::isspace(static_cast<unsigned char>(list[i]))))
      ++i;
    size_t start = i;
    while (i < list.size() && list[i] != ',' &&
           !std::isspace(static_cast<unsigned char>(list[i])))
      ++i;
    if (i == start)
      continue;
    std::string entry = list.substr(start, i - start);
    if (entry.back() == '*') {
      const std::string prefix = entry.substr(0, entry.size() - 1);
      if (ns.compare(0, prefix.size(), prefix) == 0)
        return true;
    } else if (ns == entry) {
      return true;
    }
  }
  return false;
}

// Re-render opted-in layer surfaces ON TOP of the overview: queue real
// CSurfacePassElements after the overview chrome so they composite last, above
// the backdrop/strip. (IHyprRenderer::renderLayer is protected in 0.55.4, hence
// this immediate-mode queue.)
void Overview::renderAboveLayers() const {
  const auto m = m_monitor.lock();
  if (!m || !g_pHyprRenderer)
    return;
  const auto when = Time::steadyNow();
  for (int idx : {2, 3}) {
    for (const auto &ref : m->m_layerSurfaceLayers[idx]) {
      const auto ls = ref.lock();
      if (!ls || !ls->m_mapped || !ls->wlSurface() ||
          !ls->wlSurface()->resource())
        continue;
      if (!isAboveLayer(ls->m_namespace))
        continue;

      const auto pos =
          ls->m_realPosition->value(); // absolute logical (layout) coords
      const auto size = ls->m_realSize->value();
      if (!(size.x > 0 && size.y > 0))
        continue;

      g_pHyprRenderer->damageBox(
          CBox{pos.x, pos.y, size.x,
               size.y}); // force a composite even with no client damage

      CSurfacePassElement::SRenderData data{};
      data.pMonitor = m;
      data.when = when;
      data.pos = pos;
      data.w = std::max(size.x, 1.0);
      data.h = std::max(size.y, 1.0);
      data.fadeAlpha = 1.F;
      data.alpha = 1.F;
      data.decorate = false;
      data.rounding = 0;
      data.blur = false;
      data.surfaceCounter = 0;

      const auto root = ls->wlSurface()->resource();
      root->breadthfirst(
          [&data, &root](SP<CWLSurfaceResource> s, const Vector2D &offset,
                         void *) {
            if (!s || !s->m_current.texture || s->m_current.size.x < 1 ||
                s->m_current.size.y < 1)
              return;
            data.localPos = offset;
            data.texture = s->m_current.texture;
            data.surface = s;
            data.mainSurface = s == root;
            g_pHyprRenderer->m_renderPass.add(
                makeUnique<CSurfacePassElement>(data));
            data.surfaceCounter++;
          },
          nullptr);
    }
  }
}

void Overview::renderCursorOnTop() const {
  // Hyprland composites the software cursor before our RENDER_LAST_MOMENT pass,
  // so the backdrop paints over it. Redraw on top so the pointer stays visible
  // while up.
  const auto m = m_monitor.lock();
  if (!m || !g_pPointerManager || !g_pHyprOpenGL)
    return;
  const auto tex = g_pPointerManager->getCurrentCursorTexture();
  if (!tex)
    return;
  const CBox g = g_pPointerManager->getCursorBoxGlobal();
  const CBox lb(g.x - m->m_position.x, g.y - m->m_position.y, g.w, g.h);
  g_pHyprOpenGL->renderTexture(tex, pxb(lb, m->m_scale), {.a = 1.0F});
}

double Overview::newCardScale() const {
  if (!m_newCardAnim)
    return 1.0;
  const double dur = std::max(120, cfgInt("plugin:gloview:duration", 360));
  const double p = std::clamp(nowMs(m_newCardStart) / dur, 0.0, 1.0);
  // easeOutBack — a little overshoot so the card "pops" in
  const double c1 = 1.70158, c3 = c1 + 1.0;
  const double x = p - 1.0;
  return 1.0 + c3 * x * x * x + c1 * x * x;
}

} // namespace gloview
