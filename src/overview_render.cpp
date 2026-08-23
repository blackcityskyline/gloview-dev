#include "overview.hpp"
#include "overlay_gl.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
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
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

double lerp(double a, double b, double t) { return a + (b - a) * t; }

CBox box(const LRect &r) { return CBox{r.x, r.y, r.w, r.h}; }

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
//
// The active/inactive component is recomputed here (matching
// CWindow::updateDecorationValues()'s own formula, Window.cpp) instead of
// read from the window's animated WINDOW_ALPHA_ACTIVE slot, because that slot
// is only refreshed on a real focus-change EVENT. A tile whose window sits on
// a hidden workspace never gets one while the overview is up — syncFocus()
// deliberately only calls fullWindowFocus() for a tile on the monitor's REAL
// active workspace (doing it for every hovered/selected cross-workspace tile
// would yank the visible desktop there on every mouse move). So that slot
// just keeps whatever active/inactive value real focus history last left it,
// stale by however long ago the workspace was actually visited — one window
// on a background workspace stayed rendered at its "active" windowrule
// opacity indefinitely regardless of overview navigation, while its sibling
// on the very same workspace correctly showed the "inactive" one. Keying the
// recompute off whether `w` is REALLY the live, on-screen focused window
// right now (true only on the monitor's real active workspace) instead fixes
// this: every hidden-workspace tile unconditionally renders with its
// inactive-opacity windowrule, the only value that can ever be correct for a
// window nothing is actually looking at.
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
  // NOTE: WINDOW_ALPHA_FADE is deliberately EXCLUDED here. Previews must not
  // inherit it: CSurfacePassElement multiplies data.alpha * data.fadeAlpha,
  // and any FADE contribution compounds with this function's own callers.
  // The preview fade axis belongs solely to the caller's alpha parameter.
  const float fade = w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) *
                     w->alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT);
  return std::clamp(active * fade, 0.0F, 1.0F);
}

// Exact replica of IHyprRenderer::shouldBlur(PHLWINDOW) (Renderer.cpp:3310,
// pinned 0.56.2) — that method is protected, plugins can't call it. Keep in
// sync if Hyprland changes its eligibility rules.
static bool windowBlurEligible(const PHLWINDOW &w) {
  static auto PBLUR = CConfigValue<Config::INTEGER>("decoration:blur:enabled");
  if (!*PBLUR)
    return false;
  if (w->m_ruleApplicator->noBlur().valueOrDefault() ||
      w->m_ruleApplicator->RGBX().valueOrDefault() || w->opaque())
    return false;
  const auto surface = w->wlSurface();
  if (surface && surface->m_hasBackgroundEffect)
    return !surface->m_blurRegion.empty();
  return true;
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
  const auto pos = w->positionAnimation()->goal();
  const auto size = w->sizeAnimation()->goal();
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

  // NOTE: damageWindow(w) used to be called here for every window on every
  // frame. That is a SELF-SUSTAINING render loop while the overview is open:
  // damageWindow schedules a compositor frame, the frame runs renderStage →
  // renderWindowLive for every tile → damageWindow again... → the whole overlay
  // recomposited at full refresh even with nothing changing (measured ~20% GPU
  // idle on a Haswell iGPU). Removed. Live previews stay live because the
  // CLIENT's own surface damage (a new buffer commit, video frames, caret
  // blink) already schedules the compositor frame that renderWindowLive draws
  // the fresh texture into; and the open/close/reflow/drag animations force
  // frames via Overview::damage() (renderStage's `animating` branch). Neither
  // path needs an explicit poke here.

  // ---- snapshot mode (plugin:gloview:preview_mode == "snapshot") ----
  // Render ONE CSurfacePassElement from the window's frozen texture instead of
  // walking the live surface tree. The tree walk is the per-frame cost of the
  // live path (and every sub-surface it queues); a static texture draws the
  // same tile for a tiny fraction of the GPU work — measured on a Haswell iGPU
  // with a playing video tile: overview idle went from ~20% to ~0% Render/3D
  // busy, and the open/hover/cursor animation still drives frames via the
  // usual damage() paths (snapshot textures are static, but they're drawn on
  // whatever frame the overlay renders — nothing about the compositor's
  // scheduling changes, we just stopped RE-FETCHING the live surface every
  // frame). Window content changes (video, caret, animation) simply won't
  // update the preview until the next build — that's the intended tradeoff.
  // The snapshot is the window's LAST COMMITTED main-surface texture, captured
  // at build time (updateSnapshots()); sub-surfaces (popups) aren't included.
  if (g_overview && g_overview->snapshotMode()) {
    const auto snapIt = g_overview->snapshots().find(w.get());
    if (snapIt != g_overview->snapshots().end() && snapIt->second &&
        snapIt->second->ok()) {
      CSurfacePassElement::SRenderData sdata{};
      sdata.pMonitor = mon;
      sdata.when = when;
      sdata.pos = logicalTL;
      sdata.w = std::max(size.x, 5.0);
      sdata.h = std::max(size.y, 5.0);
      sdata.surface = w->wlSurface()->resource();
      sdata.localPos = Vector2D{};
      sdata.mainSurface = true;
      sdata.texture = snapIt->second;
      sdata.dontRound = false;
      sdata.fadeAlpha = windowRealAlpha(w, mon);
      sdata.alpha = std::clamp(alpha, 0.F, 1.F);
      sdata.decorate = false;
      sdata.rounding = roundPx;
      sdata.roundingPower = roundingPower;
      sdata.blur = false; // snapshot path never spans a close glide
      sdata.pWindow = w;
      sdata.clipBox = clipPx;
      sdata.squishOversized = true;
      sdata.surfaceCounter = 0;
      g_pHyprRenderer->m_renderPass.add(makeUnique<CSurfacePassElement>(sdata));
      return;
    }
    // no captured texture yet (window appeared this frame) → fall through to
    // the live path; updateSnapshots() grabs it next build.
  }

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
  data.fadeAlpha = windowRealAlpha(w, mon);
  data.alpha = std::clamp(alpha, 0.F, 1.F);
  data.decorate = false;
  data.rounding = roundPx;
  data.roundingPower = roundingPower;
  // Real blur-behind eligibility (IHyprRenderer::shouldBlur: global enabled +
  // noblur / RGBX / opaque rules + surface background-effect state). This is
  // what makes the CLOSE transition continuous: with new_optimizations on
  // (the default) the element takes the PRECOMPUTE path and samples the
  // monitor's m_blurFB — the live blur of whatever is REALLY behind it this
  // frame — instead of our static wallpaper-only frost. Each tile's backdrop
  // then matches its post-landing state during the whole glide, so the old
  // blur -> sharp -> blur handoff cannot occur. The stale-currentWindow hazard
  // that forced blur off historically only affects the LIVE path (floating
  // windows or decoration:blur:new_optimizations=0), where the cutout box is
  // read from that global at draw time; CPreBlur runs before any surface each
  // frame and end() resets the field, so the precompute path never sees one.
  // Blur-behind samples the monitor's m_blurFB, which is built BEFORE the
  // dim/backdrop paint of the CURRENT frame — during population (appear<1)
  // or any transient alpha the sampled region is effectively undimmed and
  // flashes bright under the tile. Only fully-settled previews get it;
  // transient ones are covered by the frosted backing instead.
  // Real blur-behind (windowBlurEligible mirrors the protected shouldBlur).
  // Hyprland's own preBlur loop keeps m_blurFB self-consistent with the
  // dimmed composite, so previews and their surroundings always agree — this
  // is the verified blur->sharp->blur fix; do not add custom FB ownership
  // here again (syncMonitorBlurFB regressed it). The alpha gate exists only
  // so fading ghosts never sample an undimmed region.
  const bool wantBlur = windowBlurEligible(w) && alpha >= 0.999F;
  data.blur = wantBlur;
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
    // Immediate GL calls read Hyprland's own clipBox, scoped per queued element
    // elsewhere — clear it here so leftovers can't eat a ring edge.
    g_pHyprRenderer->m_renderData.clipBox = CBox();
    switch (m_phase) {
    case Phase::Back:
      m_owner->renderBackdrop();
      m_owner->renderPreviews(); // main tile chrome (shadow/ring/backing);
                                 // surfaces queued right after
      break;
    case Phase::Buttons:
      m_owner->renderTileButtons();
      m_owner->renderPulses(false);
      break; // per-window "✕" + swap pulses, on top of the surfaces
    case Phase::Mid:
      m_owner->renderStrip();
      break; // strip chrome; surfaces queued after
    case Phase::StripButtons:
      m_owner->renderPulses(true);
      m_owner->renderStripButtons();
      break; // per-card "✕" + drop-quadrant hint, on top
    case Phase::DragBack:
      m_owner->renderDragTile();
      break; // dragged tile chrome; surface queued after
    case Phase::Front:
      m_owner->renderCursorOnTop();
      // Re-arm the animation loop HERE, in the LAST phase of pass EXECUTION —
      // not from the renderStage build callback. damage() called during BUILD
      // landed on the CURRENT frame's already-snapshotted region and was
      // consumed by its commit: the next frame then ran with a partial/empty
      // damage (whatever unrelated source — e.g. the terminal caret — happened
      // to damage), scissored all drawing to that region, and presented a
      // freshly-allocated mostly-black buffer. The "black flash" frames.
      m_owner->rearmanim();
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

double Overview::eased() const {
  // Chrome reveal/collapse curve follows its own leaf: open while entering,
  // close while exiting (m_progress is the LINEAR clock value either way).
  return curveEval(anim(m_opening ? "open" : "close").curve, m_progress);
}

double Overview::animDuration() const {
  // Legacy shared knob: open/close/reflow leaves follow it when their own
  // _ms is unset (sentinel -1). Master-off collapses everything to 1ms.
  return animMs(m_opening ? "open" : "close",
                "plugin:gloview:duration", 360);
}

double Overview::tileProgress(int i) const {
  // Entry and reflows ride the tiles' own forward clock (m_tileClock). CLOSE
  // deliberately goes back to riding m_progress DOWN: the tile lerp then has
  // the exact same shape as the collapsing chrome (both easeOutCubic of the
  // descending progress), keeping landing and strip-collapse frame-synced.
  // Driving close on a forward clock gave tiles easeOutCubic(u) against the
  // chrome's 1-u^3 — tiles raced ahead mid-flight while the band lingered,
  // the "window lands before the strip finishes" desync.
  const double base =
      m_opening ? m_tileClock.raw(reflowDur()) : m_progress;
  const int n = static_cast<int>(m_tiles.size());
  if (n <= 1)
    return base;
  // Cascade window, deliberately tight: tiles used to fan out over up to 35%
  // of the whole duration (0.05 * n, capped at 0.35), which read as a loose
  // scatter of individually-arriving tiles rather than one cohesive motion —
  // "not smooth, not monolithic". Capped much lower now so the whole grid
  // reads as a single group gliding together, with just enough offset left
  // to still hint at depth/order rather than a dead-flat snap.
  const double spread = std::min(0.08, 0.015 * n); // total cascade window
  const double start = spread * (static_cast<double>(i) / (n - 1));
  const double span = std::max(0.001, 1.0 - spread);
  return std::clamp((base - start) / span, 0.0, 1.0);
}

double Overview::tileAppear(int i) const {
  // Same tight stagger as the position glide, on the populate clock.
  const int n = static_cast<int>(m_tiles.size());
  if (n <= 1)
    return curveEval(anim("populate").curve,
                     m_populate.raw(populateMs()));
  const double base = m_populate.raw(populateMs());
  const double spread = std::min(0.08, 0.015 * n);
  const double start = spread * (static_cast<double>(i) / (n - 1));
  const double span = std::max(0.001, 1.0 - spread);
  return curveEval(anim("populate").curve,
                   std::clamp((base - start) / span, 0.0, 1.0));
}

LRect Overview::currentBox(const Tile &t, int i) const {
  // Plain smooth deceleration instead of easeOutBack's overshoot/"pop" — the
  // bounce is a nice touch in isolation but combined with the (now much
  // shorter) stagger above it read as jerky rather than fluid, since each
  // tile's own little bounce landed at a visibly different moment. A single
  // shared curve with no overshoot is what actually reads as "monolithic".
  const double e = curveEval(anim("reflow").curve, tileProgress(i));
  const auto &a = t.natural;
  const auto &b = t.target;
  LRect r{lerp(a.x, b.x, e), lerp(a.y, b.y, e), lerp(a.w, b.w, e),
          lerp(a.h, b.h, e)};
  // population scale: a brand-new tile grows from its slot center
  const double ap = t.appear < 1.0 ? tileAppear(i) : 1.0;
  if (ap < 1.0) {
    const double k = 0.85 + 0.15 * ap;
    const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    r = LRect{cx - r.w * k / 2.0, cy - r.h * k / 2.0, r.w * k, r.h * k};
  }
  return r;
}

void Overview::updateAnimation() {
  const double dur = animDuration();
  // Stall guard FIRST: measure the gap since the previous animated frame; a
  // render hole rewinds every active clock to its last-known position so the
  // wall-time inside the hole never counts.
  const auto nowTick = std::chrono::steady_clock::now();
  if (m_lastAnimTick.time_since_epoch().count() != 0) {
    const double gapMs =
        std::chrono::duration<double, std::milli>(nowTick - m_lastAnimTick)
            .count();
    if (gapMs > 100.0) {
      m_timeline.compensateStall(gapMs, dur);
      m_tileClock.compensateStall(gapMs, reflowDur());
      if (m_newCardAnim)
        m_newCard.compensateStall(gapMs, newCardDur());
    }
  }
  m_lastAnimTick = nowTick;

  // AN5: ease the strip scroll toward its target (strip_step leaf).
  if (!m_stripTween.done(animMs("strip_step", nullptr, 200)))
    m_stripScroll = std::lerp(
        m_stripScrollFrom, m_stripScrollTarget,
        curveEval(anim("strip_step").curve,
                  m_stripTween.raw(animMs("strip_step", nullptr, 200))));
  else
    m_stripScroll = m_stripScrollTarget;

  if (m_populate.done(populateMs()) && !m_ghosts.empty())
    m_ghosts.clear();

  // Advance/prune swap pulses. Progress accumulates per animated frame with
  // the frame delta CAPPED, so a post-drop render hole cannot jump the ring
  // through its overshoot plateau (the "snaps wide and freezes" artifact).
  const double pulseMs = animMs("swap_pulse", nullptr, 180);
  const auto nowTickP = std::chrono::steady_clock::now();
  for (auto &p : m_pulses) {
    if (p.w.expired())
      continue;
    const double dt =
        std::min(34.0,
                 std::chrono::duration<double, std::milli>(nowTickP - p.last)
                     .count());
    p.last = nowTickP;
    p.p += dt / std::max(1.0, pulseMs);
  }
  std::erase_if(m_pulses,
                [](const WinPulse &p) { return p.w.expired() || p.p >= 1.0; });

  const double t = m_timeline.raw(dur);
  m_progress = m_opening ? t : 1.0 - t;
  if (m_newCardAnim && m_newCard.done(newCardDur())) {
    m_newCardAnim = false;
    m_newCardId = 0;
  }
  // Close complete: DON'T deactivate here — flipping m_active off mid-frame
  // would make renderStage skip this frame's overlay → one transparent frame
  // (real windows already suppressed). Pin progress to 0, let renderStage draw
  // the final opaque-preview frame, then deactivate once the pass is built
  // (m_pendingDeactivate).
  // Close completion needs BOTH clocks done: chrome finishes early
  // (EXIT_CHROME_SPAN), but flipping m_active off before the tiles have
  // landed would pop them mid-air — real windows reappear while previews are
  // still gliding home.
  if (!m_opening && t >= 1.0 && m_tileClock.done(dur)) {
    m_progress = 0.0;
    m_pendingDeactivate = true;
  }
}

// ---- render -----------------------------------------------------------------

void Overview::renderStage(eRenderStage stage) {
  // Pre-active probe: the black-flash frames may fire while m_active is still
  // false (or before LAST_MOMENT reaches us). For ~400ms after openStamp,
  // log every render-stage emission with the frame's damage footprint so a
  // recording can pin exact black-frame indices against these lines.
  if (!m_active && stage == RENDER_PRE) {
    const auto rm0 = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (rm0 && rm0 == m_monitor.lock()) {
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - m_openStamp)
                            .count();
      if (ms > -300.0 && ms < 400.0)
        dbg("PRE t=+" + std::to_string(ms).substr(0, 7) + "ms inactive");
    }
  }
  if (!m_active)
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
  // Frame trace for the two open bugs (debug_logs=1). One line per animated
  // frame, correlating our animation state with what Hyprland's render path
  // was doing at LAST_MOMENT time:
  //   kk     — backdrop-blit alpha this frame (pow tail on close)
  //   frost  — per-tile frosted-backing alpha (1-e)
  //   blurOK — cached blur texture present+ok (the thing Bug B hands off to)
  //   soli   — a fullscreen "solitary client" fast path was armed
  //   dso    — direct scanout was active on the output
  //   dmg    — frame damage snapshot empty / its rect count (partial frames!)
  {
    const double e   = eased();
    const float  k   = static_cast<float>(e);
    const float  kk  = m_opening ? k : std::pow(k, 0.45F);
    const auto   sol = rm->m_solitaryClient.lock();
    const bool   blurOK =
        m_blur.valid && m_blur.fb && m_blur.fb->isAllocated() &&
        m_blur.fb->getTexture() && m_blur.fb->getTexture()->ok();
    // Damage footprint: empty-flag alone hid partial-damage frames — a
    // caret-sized region renders the terminal crisp over an otherwise-stale
    // buffer, exactly the flash signature.
    const auto ext = g_pHyprRenderer->m_renderData.damage.copy().getExtents();
    dbg("F t=+" +
        std::to_string(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_openStamp)
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
        " rects=" + std::to_string(g_pHyprRenderer->m_renderData.damage.copy().getRects().size()));
  }

  updateHover(); // keep hover fresh even when the pointer is warped, not moved
  syncTiles(); // window opened/closed/moved on this workspace → reflow the grid
  updateSnapshots(); // snapshot mode: grab any tile/strip window we don't have
                     // a frozen texture for yet (cheap: contains() + refcount)

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
      // really on screen. MUST go through replayReflow: a bare rebuild here
      // reset Tile.appear to 1 and left stale ghosts from the just-started
      // populate — the visible "tiles jerk then settle" on ctrl-jump and on
      // cross-workspace LMB drops.
      dbg("WSFOLLOW ->" + std::to_string(m->m_activeWorkspace->m_id) +
          " tiles=" + std::to_string(m_tiles.size()));
      m_workspace = m->m_activeWorkspace;
      const auto shown = captureCurrentBoxes();
      buildTiles();
      buildStrip();
      layoutTiles();
      startTileGlide(shown);
      // Do NOT invalidate the blur cache here — it depends only on the
      // backdrop SOURCE (frozen wallpaper layers or a live mpv texture), not
      // on which workspace is displayed.  A workspace switch between two
      // non-mpv workspaces shares the same wallpaper source, so re-blurring
      // would introduce unnecessary per-frame variation into the cached result
      // (visible as a ~20 % brightness shift on the backdrop).  The per-frame
      // source-identity check in renderBackdrop() handles transitions to/from
      // workspaces with a featured fullscreen mpv window.
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
  renderGhosts();
  renderMainWindows();
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Buttons));
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Mid));
  renderStripWindows();
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::StripButtons));
  const bool draggingTile = draggedTile() >= 0;
  const bool draggingStripWin =
      m_drag.press == Drag::Press::StripWin && !m_drag.win.expired();
  const bool dragging = draggingTile || draggingStripWin;
  if (dragging) {
    pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::DragBack));
    renderDragWindow();
  }
  pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Front));

  renderAboveLayers(); // opted-in layer surfaces (e.g. the live-input HUD) sit
                       // on top of the overview

  // While ANY of our clocks runs, keep forcing full-monitor frames: foreign
  // half-damage frames (the real workspace-slide animation schedules them)
  // otherwise interleave and CPreBlur rebuilds only half of m_blurFB —
  // xray previews then sample two different eras split by a straight line
  // (the "tile divided in half, one side brighter" artifact).
  {
    const bool busy = secondaryAnimsActive() ||
                      !m_tileClock.done(reflowDur()) || m_newCardAnim ||
                      m_drag.lifted || !m_pulses.empty() ||
                      (m_opening ? m_progress < 1.0 : m_progress > 0.0);
    if (busy && m != nullptr)
      m->m_forceFullFrames = std::max(m->m_forceFullFrames, 1);
  }

  // Final close frame: the overlay (opaque previews at natural positions) is
  // now queued, covering the windows shouldRenderWindow suppressed earlier this
  // frame. Flip off NOW, after the pass is built, so the NEXT frame renders the
  // real windows — pixel-perfect handoff, no transparent gap. The queued
  // surfaces already captured their data; the deferred chrome callbacks no-op
  // at progress 0. deactivate() damages the next frame.
  if (m_pendingDeactivate) {
    m_pendingDeactivate = false;
    dbg("handoff: final overlay frame queued, deactivating");
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
  // m_altTabbing is deliberately NOT part of this: it lasts the whole alt-tab
  // session, and every actual change (a tab step, hover/selection change,
  // click) already calls damage() itself (stepAltTab, moveSelection,
  // updateHover, …), so nothing needs a per-frame poke on top of that.
}


SP<Render::ITexture> Overview::backdropSource(bool &live) const {
  live = false;
  const auto m = m_monitor.lock();
  if (!m)
    return nullptr;
  const auto fsBg = cfgInt("plugin:gloview:fullscreen_background", 0);
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

  // Once drawn for this overview session, just return the cached FBO texture —
  // wallpaper layers are static and a re-draw could pick up transient window
  // content captured by a wallpaper engine that screen-captures the composited
  // desktop for its rendering effects.
  if (m_backdropDrawn && !needRedraw)
    return m_backdropSrcFB->getTexture();

  // Bind the FBO as both the GL target AND the renderer's currentFB (the
  // immediate-mode renderRect/renderTexture draw into currentFB), then restore
  // both plus the GL viewport on exit.
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

  // 1. Solid background color under everything (never read currentFB — it can
  // hold a solitary fullscreen window that bypasses shouldRenderWindow).
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
  const double s = m->m_scale;
  const auto fullPx = pxb(CBox(0, 0, m->m_size.x, m->m_size.y), s);
  const int W = m->m_pixelSize.x;
  const int H = m->m_pixelSize.y;
  const double e = eased();
  const auto baseCol =
      cfgColor("backdrop_color", "0x73070a10"); // raw config literal

  // Crossfade factor. Deliberately just `e` — blur_strength keeps meaning
  // "filter radius", never a blend amount.
  //
  // There is deliberately NO snapshot of the pre-open FRAME under this fade:
  // a frozen copy containing windows ghosted full-size window copies under
  // the already-departing preview tiles ("motion trails") on entry and
  // re-showed stale windows before the tiles landed on exit. Window CONTENT
  // continuity needs no snapshot anyway: frame 1 of the animation draws each
  // preview at its natural box — pixel-identical to the just-hidden real
  // window.
  //
  // What CAN flicker is the desktop UNDER the fade: at low k the blend target
  // is whatever currentFB holds, and a transiently missing/dark wallpaper
  // (layer-shell engine redrawing, commit ordering) shows through as a dark
  // blink. So during the ENTRY fade an OPAQUE BASE built purely from the
  // backdrop SOURCES (background color + wallpaper + bg/bottom layers, or the
  // featured mpv texture) is drawn under everything — window-free, hence
  // ghost-free, and pixel-defined from frame 0. On EXIT no base: blur decays
  // over live currentFB straight into the real desktop the previews are
  // landing on.
  const float k = static_cast<float>(e);
  dbg(std::string("backdrop e=") + std::to_string(e).substr(0, 5) +
      " opening=" + std::to_string(m_opening) +
      " k=" + std::to_string(k).substr(0, 5));

  if (!blurEnabled()) {
    dbg("backdrop no-blur path");
    // ---- No-blur backdrop ----
    const auto col = argb(baseCol, e);
    if (col.a <= 0.0)
      return;
    g_pHyprOpenGL->renderRect(fullPx, col, {});
    return;
  }

  // Resolve the blur SOURCE every frame (it doubles as the entry base):
  // a fullscreen-mpv surface texture (fullscreen_background), the monitor's
  // wallpaper texture, else the frozen wallpaper-layers FBO. Comparing
  // identities invalidates only on genuine source changes while plain
  // workspace switches sharing the same wallpaper keep the cached blur.
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
  // Static desktop behind ⇒ blur ONCE into m_blur.fb, then one cheap textured
  // quad per frame, faded by k. Cache content carries NO dim and NO animation
  // (pure blur of src) — the dim rect and the fade live at the draw site.
  const void *srcId = src ? src.get() : nullptr;
  // Full cache key: source identity + live filter recipe (config changes must
  // apply without reopening the overview). liveSrc bypasses the key entirely:
  // a playing video must re-blur EVERY frame even if the driver hands us the
  // same buffer texture twice in a row.
  const int cPasses = blurPasses();
  const int cSize = blurSize();
  const int cRes = blurResolution();
  const float cStrength = blurStrength();
  if (liveSrc || !m_blur.matches(srcId, cPasses, cSize, cRes, cStrength))
    m_blur.invalidate();
  m_blur.srcId = srcId;
  m_blur.passes = cPasses;
  m_blur.sizePx = cSize;
  m_blur.resolution = cRes;
  m_blur.strength = cStrength;

  if (!m_blur.valid || !m_blur.fb || !m_blur.fb->isAllocated() ||
      m_blur.fb->m_size != Vector2D(W, H)) {
    // (Re)allocate the cache at the monitor's pixel size.
    if (!m_blur.fb)
      m_blur.fb = g_pHyprRenderer->createFB("gloview blur");
    if (!m_blur.fb->isAllocated() || m_blur.fb->m_size != Vector2D(W, H))
      m_blur.fb->alloc(W, H);

    // The blur filter manages projection/fbSize/viewport for its intermediate
    // FBOs; we hold the surrounding state so the rest of the frame is intact.
    // RPT_EXPORT + fbSize=(W,H) is also what the backdrop source render (just
    // above) and the blur itself need for pixel coords.
    const auto oldProjType = g_pHyprRenderer->m_renderData.projectionType;
    const auto oldFbSize = g_pHyprRenderer->m_renderData.fbSize;
    g_pHyprRenderer->m_renderData.fbSize = Vector2D(W, H);
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);

    // Deliberately do NOT read currentFB as the source, for two reasons:
    // (1) a fullscreen window on the active workspace renders through
    // Hyprland's "solitary client" fast path in renderMonitor, which calls
    // renderWindow DIRECTLY — bypassing shouldRenderWindow entirely — so
    // currentFB holds that window; (2) a workspace that never re-renders
    // leaves currentFB holding a stale pre-overview frame.
    m_blurFilter.prepare(cPasses, static_cast<float>(cSize), cRes, cStrength);
    const bool ok = src && m_blurFilter.render(src, m_blur.fb, W, H);

    // Restore the renderer state we borrowed.
    g_pHyprRenderer->m_renderData.fbSize = oldFbSize;
    g_pHyprRenderer->setProjectionType(oldProjType);
    const auto PMON = g_pHyprRenderer->m_renderData.pMonitor;
    g_pHyprOpenGL->setViewport(0, 0, PMON ? (int)PMON->m_pixelSize.x : W,
                               PMON ? (int)PMON->m_pixelSize.y : H);

    if (!ok) {
      // No source at all, or blur unavailable — never leave currentFB exposed
      // (it can hold a solitary fullscreen window that bypasses
      // shouldRenderWindow, e.g. during a workspace-switch animation): cover
      // everything with an OPAQUE background rect + dim overlay, and retry
      // next frame.
      m_blur.valid = false;
      dbg("backdrop FALLBACK: blur unavailable / no source");
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
    // Cache unavailable — never leave the base/currentFB exposed (see
    // blur-fail path above).  Draw an opaque background rect instead.
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
      argb(cfgColor("strip_band_color", "0x24ffffff"), e);
  const bool blur = false; // strip band never uses native blur: it samples
                           // currentFB which can hold a solitary fullscreen
                           // window (the workspace-switch fast path bypasses
                           // shouldRenderWindow), leaking window content into
                           // the band's blurred overlay.  The self-blur
                           // backdrop already handles the blur behind the
                           // strip — a flat band colour is sufficient.
  const LRect bandR = stripBand();
  const Vector2D slide =
      stripSlide(e); // slide the whole strip in from its edge
  const Vector2D scroll = stripScroll(); // scroll the card group along the band
  g_pHyprOpenGL->renderRect(
      pxb(CBox(bandR.x + slide.x, bandR.y + slide.y, bandR.w, bandR.h), s),
      bandCol, {.blur = blur, .blurA = static_cast<float>(e) * blurStrength()});

  const int cardRound = cfgInt("plugin:gloview:strip_card_round", 10);
  const auto cardBg = argb(cfgColor("strip_card_color", "0x3a0e131c"), e);
  const auto activeBg =
      argb(cfgColor("strip_active_color", "0x4d1c2c44"), e);
  const auto activeLine =
      argb(cfgColor("strip_active_border", "0xf0ffffff"), e);
  const auto hoverLine =
      argb(cfgColor("strip_hover_border", "0x80ffffff"), e);
  const auto plusCol =
      argb(cfgColor("strip_plus_color", "0xd0eef4ff"), e);
  const auto allCol = argb(cfgColor("strip_all_color", "0xd0eef4ff"),
                           e); // own key, no longer plusCol
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
    if (m_newCardAnim && it.id == m_newCardId && it.kind != StripItem::Kind::Plus && it.kind != StripItem::Kind::All) {
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
        it.active || (allWs && allCardShown && it.kind == StripItem::Kind::All); // filled + thick ring
    const bool expoRing =
        allWs && !allCardShown && it.kind != StripItem::Kind::Plus; // outline-all fallback
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

    if (it.kind == StripItem::Kind::Plus) {
      // draw a centered plus
      const double t = std::max(2.0, card.h * 0.04);
      const double L = std::min(card.w, card.h) * 0.34;
      const double cx = card.cx(), cy = card.cy();
      g_pHyprOpenGL->renderRect(pxb(CBox(cx - L / 2, cy - t / 2, L, t), s),
                                plusCol, {.round = pxr(t / 2, s)});
      g_pHyprOpenGL->renderRect(pxb(CBox(cx - t / 2, cy - L / 2, t, L), s),
                                plusCol, {.round = pxr(t / 2, s)});
    } else if (it.kind == StripItem::Kind::All) {
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
              allCol, {.round = pxr(2, s)});
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
        const LRect wbL = stripWinSlotRect(it, card, j);
        const int wRound = clampRound(previewRound, wbL.w, wbL.h);
        // Grab indicator (task #6): a bright highlight around the exact slot
        // that's currently pressed, before it's lifted into a floating drag — a
        // static highlight rather than a blink/pulse so it doesn't need to
        // force continuous repainting while the mouse just sits still holding
        // the button down.
        const bool grabbed = static_cast<int>(i) == m_drag.idx &&
                             static_cast<int>(j) == m_drag.winIdx &&
                             !(m_drag.press == Drag::Press::StripWin);
        if (grabbed)
          strokeRing(wbL, s, argb(cfgColor("hover_border", "0xf0ffffff"), e),
                     2, wRound, roundPow);
        // Thin near-invisible backing — see drawPreviewTile's comment
        // (overview_tiles_render.cpp) for the full reasoning: this used to be a
        // flat opaque/semi-opaque tint standing in for "whatever's really
        // behind the window", which is exactly what made transparent previews
        // look more solid than the real desktop. The window's own config-driven
        // alpha (fadeAlpha, computed the same way as any real Hyprland window)
        // now blends almost entirely against the strip band's own already-drawn
        // content instead.
        safetyBacking(wbL, s, cfgColor("backing_color", "0xff14181f"),
                      0.08 * e, wRound, roundPow);
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
      m_drag.armed() && m_drag.lifted;
  // Carrying a STRIP window: hovering an exact slot means swap intent —
  // real-slot rings for ANY button (both swap on slot drop); insert-zone
  // hints only apply away from slots.
  const bool rmbSwap =
      dropping && m_drag.press == Drag::Press::StripWin;

  // RMB swap-drag: ring the SOURCE slot once, wherever it lives.
  if (rmbSwap && m_drag.idx >= 0 &&
      m_drag.idx < static_cast<int>(m_strip.size()) &&
      m_drag.winIdx < static_cast<int>(m_strip[m_drag.idx].wins.size())) {
    strokeRingPx(pxb(stripWinSlotRect(m_strip[m_drag.idx],
                                      stripCardAt(m_drag.idx),
                                      m_drag.winIdx),
                     s),
                 argb(cfgColor("select_border", "0xf066ccff"), e),
                 0.7F * static_cast<float>(e),
                 clampRound(cfgInt("plugin:gloview:preview_round", 12), 40,
                            40),
                 roundPow);
  }

  for (size_t i = 0; i < m_strip.size(); ++i) {
    const auto &it = m_strip[i];
    if (it.kind == StripItem::Kind::Plus || it.kind == StripItem::Kind::All)
      continue;
    const LRect card = stripCardAt(i);

    // Destination hints. LMB drag (insert): exactly TWO scenarios — whole
    // card (empty ws) or halves by the first window's aspect. RMB drag while
    // carrying a STRIP window (swap): no insert zones at all — instead the
    // REAL windows are highlighted where they actually sit: a ring on the
    // exact hovered slot (the swap partner) plus a ring on the source slot.
    // partner-ring intent: RMB on any slot, or LMB on a slot of the dragged
    // window's OWN workspace (intra-tile swap)
    bool intentRing = false;
    if (dropping && m_drag.press == Drag::Press::StripWin &&
        static_cast<int>(i) == m_hoveredStrip && !it.wins.empty() &&
        m_drag.idx >= 0 && m_drag.winIdx >= 0) {
      const auto dragW = m_drag.win.lock();
      for (size_t j = 0; j < it.wins.size(); ++j) {
        const auto v = it.wins[j].win.lock();
        if (!v || v == dragW)
          continue;
        if (!stripWinSlotRect(it, card, j).contains(m_drag.x, m_drag.y))
          continue;
        intentRing =
            rmbSwap ||
            (dragW && v->m_workspace == dragW->m_workspace);
        break;
      }
    }
    if (intentRing && !it.wins.empty()) {
      const auto hoverCol =
          argb(cfgColor("hover_border", "0xf0ffffff"), e);
      for (size_t j = 0; j < it.wins.size(); ++j) {
        const auto v = it.wins[j].win.lock();
        if (!v || v->m_isMapped == false)
          continue;
        const LRect sl = stripWinSlotRect(it, card, j);
        if (!sl.contains(m_drag.x, m_drag.y))
          continue;
        strokeRingPx(pxb(sl, s), hoverCol, 0.9F * static_cast<float>(e),
                     clampRound(cfgInt("plugin:gloview:preview_round", 12),
                                sl.w, sl.h),
                     roundPow);
        break;
      }
    } else if (!rmbSwap && dropping &&
               static_cast<int>(i) == m_hoveredStrip) {
      LRect zone = card;
      if (!it.wins.empty()) {
        const auto &r = it.wins.front().rel;
        if (r.w * card.w >= r.h * card.h) // wide → left/right halves
          zone = (m_drag.x < card.cx()) ? LRect{card.x, card.y, card.w / 2.0, card.h}
                                       : LRect{card.cx(), card.y, card.w / 2.0, card.h};
        else                              // tall → top/bottom halves
          zone = (m_drag.y < card.cy()) ? LRect{card.x, card.y, card.w, card.h / 2.0}
                                       : LRect{card.x, card.cy(), card.w, card.h / 2.0};
      }
      g_pHyprOpenGL->renderRect(
          pxb(zone, s), argb(cfgColor("drop_hint_color", "0x38ffffff"), e),
          {.round = pxr(cardRound / 2, s), .roundingPower = roundPow});
    }

    // "close every window on this workspace" — the visible counterpart to the
    // middle-click shortcut, shown under the same visibility rule as the
    // per-window "✕". Stays a plain circle (default roundingPower) regardless
    // of preview_round_power — a "squircle" close button would look broken at
    // non-default curve exponents.
    if (showClose) {
      const LRect br = closeButtonRect(card);
      const double rad = br.w / 2.0;
      g_pHyprOpenGL->renderRect(
          pxb(box(br), s),
          argb(cfgColor("close_button_color", "0xe6e23b3b"), e),
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
    if (it.kind == StripItem::Kind::Plus || it.kind == StripItem::Kind::All) // neither carries window previews
      continue;
    LRect card = it.card;
    card.x += slide.x + scroll.x;
    card.y += slide.y + scroll.y;
    for (size_t j = 0; j < it.wins.size(); ++j) {
      // being dragged as a floating preview right now → drawn separately, skip
      // here
      if (m_drag.press == Drag::Press::StripWin &&
          static_cast<int>(i) == m_drag.idx &&
          static_cast<int>(j) == m_drag.winIdx)
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
      renderWindowLive(w, m, slotPx, cardPx,
                       static_cast<float>(e), when, round,
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
          ls->positionAnimation()->value(); // absolute logical (layout) coords
      const auto size = ls->sizeAnimation()->value();
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
  // Delegate to the cursor module: a hardware cursor (KMS plane) is
  // composited above the framebuffer by the display hardware, so it's always
  // visible over the dim backdrop with zero framebuffer writes — no redraw,
  // no erase, no trails, zero GPU cost per move. Only when no HW cursor is
  // available does the module draw a software cursor (with an opaque erase
  // rect over the previous position to prevent trails).
  const auto m = m_monitor.lock();
  if (!m)
    return;
  m_cursor.renderOnTop(
      m, argb(cfgColor("backdrop_color", "0x73070a10"), 1.0));
}

void Overview::animateStripTo(double from, double to) {
  from = std::clamp(from, 0.0, std::max(0.0, m_stripScrollMax));
  to = std::clamp(to, 0.0, std::max(0.0, m_stripScrollMax));
  const auto a = anim("strip_step");
  if (!a.on || std::abs(to - from) < 0.5) {
    m_stripScrollFrom = to;
    m_stripScrollTarget = to;
    m_stripScroll = to;
    return;
  }
  // retarget mid-flight: continue from wherever the eased value is NOW so
  // rapid wheel notches read as one continuous scrub
  m_stripScrollFrom =
      !m_stripTween.done(a.ms) ? m_stripScroll : from;
  m_stripScrollTarget = to;
  m_stripTween.begin();
  ensureAnimPump();
}

void Overview::kickPulse(const PHLWINDOW &w) {
  if (!w || !anim("swap_pulse").on)
    return;
  std::erase_if(m_pulses, [&w](const WinPulse &p) { return p.w.lock() == w; });
  m_pulses.push_back(
      WinPulse{w, 0.0, std::chrono::steady_clock::now()});
}

// Removed-by-rebuild tiles fading/scaling out where they were (all→one,
// close-window). Same populate clock as Tile.appear — the mirror direction.
void Overview::renderGhosts() const {
  if (m_ghosts.empty() || m_populate.done(populateMs()))
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double scale = m->m_scale;
  const auto when = Time::steadyNow();
  const int round = pxr(cfgInt("plugin:gloview:preview_round", 12), scale);
  const float roundPow = cfgFloat("plugin:gloview:preview_round_power", 2.0F);
  // Softer than the incoming pop: ghosts are a motion cue, not the main
  // event — shorter and starting semi-transparent so all->one collapse
  // reads as survivors dispersing, not as duplicates lingering.
  const double p = std::min(1.0, m_populate.raw(populateMs()) / 0.6);
  const double eOut = curveEval(anim("populate").curve, p); // 0..1 gone
  for (const auto &g : m_ghosts) {
    const auto w = g.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
      continue;
    const double k = 1.0 - 0.15 * eOut; // shrink toward its own center
    const double cx = g.box.x + g.box.w / 2.0, cy = g.box.y + g.box.h / 2.0;
    const LRect box{cx - g.box.w * k / 2.0, cy - g.box.h * k / 2.0,
                    g.box.w * k, g.box.h * k};
    const CBox px(box.x * scale, box.y * scale, box.w * scale,
                  box.h * scale);
    renderWindowLive(w, m, px, px, static_cast<float>((1.0 - eOut) * 0.65),
                     when, round, roundPow);
  }
}

// Active swap pulses. strip=true → ring the window's STRIP slot; false → its
// GRID tile box. A window present in both gets pulsed on both surfaces.
void Overview::renderPulses(bool strip) const {
  if (m_pulses.empty())
    return;
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const double s = m->m_scale;
  const float roundPow = cfgFloat("plugin:gloview:preview_round_power", 2.0F);
  const auto col = argb(cfgColor("hover_border", "0xf0ffffff"), 1.0);
  for (const auto &p : m_pulses) {
    const auto w = p.w.lock();
    if (!w)
      continue;
    const double pr = std::clamp(p.p, 0.0, 1.0);
    if (strip) {
      for (size_t i = 0; i < m_strip.size(); ++i) {
        const auto &it = m_strip[i];
        if (it.kind != StripItem::Kind::Ws)
          continue;
        for (size_t j = 0; j < it.wins.size(); ++j) {
          if (it.wins[j].win.lock() != w)
            continue;
          const LRect card = stripCardAt(i);
          const LRect sl = stripWinSlotRect(it, card, j);
          strokeRingPx(pxb(sl, s), col, static_cast<float>((1.0 - pr) * 0.9),
                       clampRound(cfgInt("plugin:gloview:preview_round", 12),
                                  sl.w, sl.h),
                       roundPow);
        }
      }
    } else {
      for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (m_tiles[i].win.lock() != w)
          continue;
        const LRect lb =
            tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        drawPulseRing(pxb(lb, s), pxr(cfgInt("plugin:gloview:preview_round", 12), s),
                      roundPow, col, pr);
        break;
      }
    }
  }
}

// Expanding/fading ring flash around a box at pulse progress p (0..1):
// easeOutBack pushes the outline slightly past the box, then it settles back
// while fading out.
void Overview::drawPulseRing(const CBox &boxPx, int round, float roundPow,
                             const CHyprColor &col, double p) const {
  const double k = curveEval(Curve::Back, p) * 0.10;
  const double cx = boxPx.x + boxPx.w / 2.0, cy = boxPx.y + boxPx.h / 2.0;
  const CBox ring{cx - boxPx.w * (0.5 + k), cy - boxPx.h * (0.5 + k),
                  boxPx.w * (1.0 + 2.0 * k), boxPx.h * (1.0 + 2.0 * k)};
  strokeRingPx(ring, col, static_cast<float>((1.0 - p) * 0.9),
               static_cast<int>(std::max(2.0, round + round * 0.5 * k)), roundPow);
}

double Overview::newCardScale() const {
  if (!m_newCardAnim)
    return 1.0;
  const double p = m_newCard.raw(newCardDur());
  // pop curve from the registry ("back" default — a little overshoot)
  return curveEval(anim("new_card").curve, p);
}

} // namespace gloview
