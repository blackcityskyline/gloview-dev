#include "overview.hpp"
#include "overlay_gl.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

// Defined (external linkage) in overview_render.cpp; forward-declared here rather than
// duplicated since both TUs need the exact same implementation.
void renderWindowLive(const PHLWINDOW& w, const PHLMONITOR& mon, const CBox& destPx, const CBox& clipPx, float alpha, const Time::steady_tp& when, int roundPx = 0, float roundingPower = 2.0F, bool execCtx = false);
bool windowBlurEligible(const PHLWINDOW &w); // defined in overview_render.cpp

namespace {

LRect fitInside(const LRect& outer, double aspect) {
    if (outer.w <= 0.0 || outer.h <= 0.0 || aspect <= 0.0)
        return outer;

    const double outerAspect = outer.w / outer.h;
    if (std::abs(outerAspect - aspect) <= 0.01)
        return outer;

    if (outerAspect > aspect) {
        const double w = outer.h * aspect;
        return LRect{outer.x + (outer.w - w) / 2.0, outer.y, w, outer.h};
    }

    const double h = outer.w / aspect;
    return LRect{outer.x, outer.y + (outer.h - h) / 2.0, outer.w, h};
}

} // namespace

LRect Overview::closeButtonRect(const LRect& lb) const {
    const double scale = std::max(0.3, static_cast<double>(cfgFloat("plugin:gloview:close_button_size", 1.0F)));
    const double r      = std::clamp(std::min(lb.w, lb.h) * 0.11, 9.0, 18.0) * scale;
    const double inset  = r + 6.0;
    const std::string pos = cfgStr("plugin:gloview:close_button_position", "top-right");
    double cx = lb.x + lb.w - inset, cy = lb.y + inset; // top-right (default)
    if (pos == "top-left") {
        cx = lb.x + inset;
        cy = lb.y + inset;
    } else if (pos == "bottom-right") {
        cx = lb.x + lb.w - inset;
        cy = lb.y + lb.h - inset;
    } else if (pos == "bottom-left") {
        cx = lb.x + inset;
        cy = lb.y + lb.h - inset;
    }
    return LRect{cx - r, cy - r, 2 * r, 2 * r};
}

// plugin:gloview:close_button_visibility == "always" — show close buttons on every tile
// and strip card all the time, not just in desktop mode / while Shift is the desktop-mode
// key (task #9).
bool Overview::closeButtonsAlwaysOn() const {
    return cfgStr("plugin:gloview:close_button_visibility", "shift") == "always";
}

// plugin:gloview:close_trigger == "doubleclick" (default "button"): swaps the per-window "✕"
// entirely for a double-click/double-tap directly on the tile — see the deferred single-click
// handling in onMouseButton. Only affects the PER-WINDOW close mechanism; the strip card's
// whole-workspace "✕"/middle-click and the keyboard key_close_window are unrelated and unaffected.
bool Overview::closeOnDoubleClick() const {
    return cfgStr("plugin:gloview:close_trigger", "button") == "doubleclick";
}

void Overview::drawPreviewTile(size_t i, const LRect& slot, bool lift) const {
    const auto m = m_monitor.lock();
    if (!m || i >= m_tiles.size())
        return;
    const double s         = m->m_scale; // logical→pixel; Hyprland's renderRect wants pixel coords
    const double e         = eased();
    const int    round     = cfgInt("plugin:gloview:preview_round", 12);
    const float  roundPow  = cfgFloat("plugin:gloview:preview_round_power", 2.0F);
    const auto   shadowCol = argb(cfgColor("shadow_color", "0x70000000"), 1.0);
    const auto   hoverCol  = argb(cfgColor("hover_border", "0xf0ffffff"), e);

    const auto& t = m_tiles[i];
    const auto  w = t.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
        return;

    // Tile box fitted to the window's real aspect; live surface fills it at uniform scale so
    // backing/border/content share one rect and can't stretch.
    const LRect lb = tileContentBox(i, slot);

    // soft drop shadow. renderRoundedShadow is a real gaussian; a blurred dark rect samples
    // the backdrop blur and reads as a murky halo. roundingPower matches the live content
    // (renderMainWindows) instead of being hardcoded to a circle — otherwise the shadow's
    // corner curve visibly disagreed with the window content's at any non-default
    // preview_round_power, most noticeable right where the close button sits (task #4).
    //
    // This box is the SAME size as the tile itself (lb.w × lb.h), just shifted down by dy —
    // meaning its solid, unblurred CORE sits almost entirely within the tile's own footprint,
    // not just in the outward-extending halo a real drop shadow would occupy. That was
    // invisible for as long as an opaque backing sat between it and the live surface (it
    // fully hid the core, leaving only the halo peeking past the tile's edges, the intended
    // look) — now that the backing is nearly transparent (see its own comment above) so real
    // window transparency can show through, this shadow's core shows through right along
    // with it: a distinct darker rectangle, offset down, floating "inside" the window — not a
    // rendering bug, just this shape becoming visible for the first time. Cut hard here
    // (0.9 → 0.18) rather than reshaping the box/range geometry blind (no way to verify the
    // result visually from here) — still gives opaque windows a faint depth cue in the halo,
    // without being strong enough to read as a second layer through a transparent one.
    dropShadow(lb, s, shadowCol, lift ? 14.0 : 6.0, lift ? 30.0 : 16.0,
               e * 0.18F, round, roundPow);

    const bool framed   = (static_cast<int>(i) == m_hovered || lift);
    const bool selected = (static_cast<int>(i) == m_selected) && !lift; // keyboard-nav cursor

    // Real border STROKE (Hyprland's own renderBorder — the same call the compositor uses to
    // draw a window's own border decoration), not a filled rect grown by the line width. A
    // filled underlay relied on the live surface drawn on top being fully OPAQUE to hide
    // everything but its own outward-peeking edge; against a transparent window that
    // assumption breaks and the "ring" reads as a solid white/blue wash covering the whole
    // preview instead of a frame around it. renderBorder shades only the ring's own pixels
    // (it explicitly subtracts the inner box from the paint region), so it can never fill
    // behind the content no matter how transparent the window is. `lb` is passed UN-grown —
    // renderBorder expands it outward by borderSize itself, same as it does for a real window
    // border.
    //
    // Two independent, modular layers, each with its own on/off, color (manual or
    // scheme-sourced), and thickness — combine freely instead of one fixed look:
    //   show_border        — a base ring on EVERY tile, all the time
    //   show_focus_border   — the hover/keyboard-selection ring on TOP of it (hover wins over
    //                        the coincident keyboard-selection ring)
    // off+off = no borders at all; off+on = focus-only (the original look); on+off = a
    // constant ring that never changes; on+on = a constant ring whose color/thickness
    // effectively changes on focus, since the focus ring draws right over it.
    if (cfgInt("plugin:gloview:show_border", 0) != 0) {
        strokeRing(lb, s, argb(cfgColor("border_color", "0x50ffffff"), e),
                   cfgInt("plugin:gloview:border_size", 2), round, roundPow);
    }
    if (cfgInt("plugin:gloview:show_focus_border", 1) != 0) {
        if (framed) // hovered (or lifted): focus ring wins over selection
            strokeRing(lb, s, hoverCol,
                       cfgInt("plugin:gloview:hover_border_size", 3), round,
                       roundPow);
        else if (selected)
            strokeRing(lb, s, argb(cfgColor("select_border", "0xf066ccff"), e),
                       cfgInt("plugin:gloview:select_border_size", 3), round,
                       roundPow);
    }

    // Thin near-invisible backing — kept ONLY as a safety margin for the 1-3px edge-seam
    // case renderWindowLive's over-cover comment describes, not to hide the window's own
    // transparency. At 1.0 (original) or even 0.7 (previous attempt) this is exactly what
    // made windows read as more opaque than the real desktop: whatever alpha the window
    // itself doesn't cover was blending against a flat dark color WE invented instead of
    // whatever's actually behind it. The window's real, config-driven transparency
    // (windowRealAlpha() below, computing active/inactive/fullscreen opacity + fade exactly
    // like the real compositor does — already correct, this was never the problem) now
    // blends almost entirely against our own backdrop, which is ALREADY correctly blurred
    // and already drawn (renderBackdrop() runs before renderPreviews() calls into this
    // function) — so a window shows up with the same transparency Hyprland itself would give
    // it, not an approximation. The 1px inset + near-invisible alpha rationale
    // lives on safetyBacking() in overlay_gl.hpp (shared with every other
    // chrome site); the color itself is plugin:gloview:backing_color.

    // Frosted backing during the transition: until the fullscreen blurred
    // backdrop is opaque (e < 1), a translucent tile's see-through pixels
    // would blend against SHARP wallpaper from currentFB, while the pre-open
    // desktop showed them through Hyprland's per-window decoration blur —
    // switching between those in one frame is the entry "blink" (visible
    // exactly on windows that have blur-behind: translucent terminals,
    // Nautilus; invisible on opaque ones and full-opaque-hint surfaces).
    // Sample OUR cached fullscreen blur into the tile box instead — the same
    // source the settled backdrop blits — so the tile reads identically from
    // frame 1 and the global backdrop then dissolves over it seamlessly.
    const double apNow =
        t.appear < 1.0 ? tileAppear(static_cast<int>(i)) : 1.0;
    // STEP C: eligible translucent tiles carry the frost underlay for their
    // WHOLE session — interior becomes pixel-identical to the surrounding
    // backdrop (cached blur + dim), independent of sibling previews.
    const bool frostAlways = w && windowBlurEligible(w) &&
        cfgInt("plugin:gloview:frost_underlay", 1) != 0;
    if (frostAlways || e < 0.999 || apNow < 0.999) {
        if (const auto btex = backdropBlurTexture(); btex && btex->ok()) {
            const CBox monPx{0.0, 0.0, m->m_size.x * s, m->m_size.y * s};
            // Alpha (1 - e): the backing hands over to the global backdrop
            // CONTINUOUSLY. A fixed 1.0 made the tile region brighter than
            // its surroundings while active (extra un-dimmed blur layer) and
            // snapped darker the moment the backing stopped at e >= 0.999 —
            // the one-frame "dim step" at animation end.
            g_pHyprOpenGL->scissor(pxb(lb, s));
            g_pHyprOpenGL->renderTexture(
                btex, monPx,
                {.a = frostAlways
                          ? 1.0F
                          : static_cast<float>(
                                std::max(1.0 - e, 1.0 - apNow))});
            // the cached blur carries NO dim — reapply it here EXACTLY ONCE so
            // the tile interior matches the dimmed surroundings to the pixel
            // (a second stacked dim made eligible translucent tiles visibly
            // darker than ineligible neighbors — the "one of two terminals
            // is darker" artifact)
            g_pHyprOpenGL->renderRect(
                pxb(lb, s),
                argb(cfgColor("backdrop_color", "0x73070a10"), 1.0), {});
            g_pHyprOpenGL->scissor(nullptr);
        }
    }
    safetyBacking(lb, s, cfgColor("backing_color", "0xff14181f"), 0.08, round,
                  roundPow);

    // window title in a dark pill below the tile (on hover or keyboard selection)
    if ((framed || selected) && !lift && t.label && t.label->m_size.x > 0) {
        const double lw   = t.label->m_size.x;
        const double lh   = t.label->m_size.y;
        const double padX = 14.0, padY = 6.0;
        const double pw   = lw + 2 * padX;
        const double ph   = lh + 2 * padY;
        const double px   = std::clamp(lb.cx() - pw / 2.0, 6.0, m->m_size.x - pw - 6.0);
        const double py   = std::min(lb.y + lb.h + 10.0, m->m_size.y - ph - 6.0);
        g_pHyprOpenGL->renderRect(pxb(CBox(px, py, pw, ph), s), argb(cfgColor("title_pill_color", "0xcc11151c"), e), {.round = pxr(ph / 2.0, s)});
        g_pHyprOpenGL->renderTexture(t.label, pxb(CBox(px + padX, py + padY, lw, lh), s), {.a = static_cast<float>(e)});
    }
}

// Fit `slot` to the window's real aspect so the live surface fills it exactly (uniform scale).
// Used by both tile chrome and the queued surface so they coincide.
LRect Overview::tileContentBox(size_t i, const LRect& slot) const {
    // Desktop (canvas) mode: slot already carries the window's aspect (the parked
    // survivors). Use it AS-IS, not the live aspect, so a survivor Hyprland re-tiled to a new
    // shape keeps its frozen preview shape instead of reshaping inside the frozen slot.
    if (m_desktopMode)
        return slot;
    double aspect = slot.w / std::max(1.0, slot.h);
    if (i < m_tiles.size()) {
        if (const auto w = m_tiles[i].win.lock()) {
            const auto s = w->sizeAnimation()->goal();
            if (s.x > 0 && s.y > 0)
                aspect = s.x / s.y;
        }
    }
    return fitInside(slot, aspect);
}

LRect Overview::dragBox() const {
    const int dragIdx = draggedTile();
    if (dragIdx < 0)
        return LRect{0, 0, 0, 0};
    // Grid mode shrinks to half AND sits offset down-right of the actual cursor position —
    // a preview sitting directly under the pointer otherwise hides the very drop-zone
    // indicator (workspace card highlight / destination quadrant hint) it's meant to be
    // aimed at, making precise placement hard to judge. Hit-testing on release still uses
    // the real cursor position; only the VISUAL sits offset. Desktop/canvas mode keeps the
    // true 1:1 grab offset and full size instead — there a drop PARKS the window at exactly
    // that visual spot, so offsetting it would leave it somewhere the user never saw.
    const LRect  base = m_tiles[dragIdx].target;
    if (m_desktopMode)
        return LRect{m_drag.x - m_drag.grabDX, m_drag.y - m_drag.grabDY,
                     base.w, base.h};
    const double w = base.w * 0.5;
    const double h = base.h * 0.5;
    constexpr double offX = 46.0, offY = 64.0; // clear of the cursor/hotspot, down-right
    return LRect{m_drag.x + offX, m_drag.y + offY, w, h};
}

void Overview::renderPreviews() const {
    const int dragIdx = draggedTile();
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == dragIdx)
            continue; // the dragged tile floats over the strip; drawn later in renderDragTile()
        drawPreviewTile(i, currentBox(m_tiles[i], static_cast<int>(i)), false);
    }
}

// Per-window "✕" — shown in desktop (canvas) mode, or always with
// close_button_visibility=always (task #9). Drawn in its OWN phase, AFTER renderMainWindows()
// has queued the live surfaces (see the Phase::Buttons comment on COverlayPass): the button
// sits INSIDE the tile bounds, so drawing it before the surface let opaque window content
// paint straight over it — it only showed through on windows with a transparent corner,
// which looked like "it works for some apps but not others" (task #3).
void Overview::renderTileButtons() const {
    if (closeOnDoubleClick())
        return; // doubleclick mode replaces the per-window "✕" entirely
    if (!m_desktopMode && !closeButtonsAlwaysOn())
        return;
    const auto m = m_monitor.lock();
    if (!m)
        return;
    // Defensive reset before drawing the ✕ glyph, fully restored below. The actual "black
    // square inside the circle" cause was GL_BLEND being left disabled by CBlurFilter::render()
    // (see its fix in blur.cpp) — with blend off, the glyph texture's transparent padding
    // around the ✕ wrote straight through as opaque black instead of blending against the red
    // circle beneath it. A GL_TEXTURE_2D-only unbind (the old fix here) never touched blend and
    // so never helped; kept below as a harmless, cheap safety net against any similar
    // leftover-texture-binding issue, known or not. Blend state (both the enable flag and the
    // exact func) is saved and put back before returning — this phase runs right before
    // renderStrip() (Phase::Mid), and leaving OUR OWN straight-alpha func in place unconditionally
    // would have been exactly the same class of leak this comment describes fixing, just handed
    // to the strip band/cards instead of the glyph.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    const bool blendBefore = glIsEnabled(GL_BLEND) == GL_TRUE;
    GLint      blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const double s      = m->m_scale;
    const double e      = eased();
    const int    dragIdx = draggedTile();
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == dragIdx)
            continue;
        const auto w = m_tiles[i].win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        const LRect lb = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        const LRect br = closeButtonRect(lb);
        g_pHyprOpenGL->renderRect(pxb(br, s), argb(cfgColor("close_button_color", "0xe6e23b3b"), e), {.round = pxr(br.h / 2.0, s)});
        if (m_closeGlyph && m_closeGlyph->m_size.x > 0) {
            const double gw = m_closeGlyph->m_size.x, gh = m_closeGlyph->m_size.y;
            const double gs = std::min((br.w * 0.62) / std::max(1.0, gw), (br.h * 0.62) / std::max(1.0, gh));
            const double dw = gw * gs, dh = gh * gs;
            g_pHyprOpenGL->renderTexture(m_closeGlyph, pxb(CBox(br.x + (br.w - dw) / 2.0, br.y + (br.h - dh) / 2.0, dw, dh), s), {.a = static_cast<float>(e)});
        }
    }
    glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
    if (!blendBefore)
        glDisable(GL_BLEND);
}

// Queues the LIVE surfaces for the main-area tiles (except the dragged one), above their
// chrome (the Back phase) and under the strip. Mirrors renderStripWindows.
void Overview::renderMainWindows() const {
    const auto m = m_monitor.lock();
    if (!m)
        return;
    // Previews render fully OPAQUE the whole time (only backdrop/strip chrome fade with `e`).
    // At progress 0, currentBox == t.natural == the real window's settled geometry, so the
    // opaque preview overlays it pixel-perfect. Fading with `e` would flicker the close tail:
    // preview alpha hits 0 while the real window is still hidden → desktop shows through.
    const int    dragIdx = draggedTile();
    const double scale   = m->m_scale;
    const auto   when    = Time::steadyNow();
    const int    round   = pxr(cfgInt("plugin:gloview:preview_round", 12), scale);
    const float  roundPow = cfgFloat("plugin:gloview:preview_round_power", 2.0F);
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == dragIdx)
            continue;
        const auto w = m_tiles[i].win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        const LRect lb = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        const CBox  px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
        // Surface alpha stays 1.0 ALWAYS: an alpha<1 surface element flips
        // CSurfacePassElement into blended/blur-gate territory (needsLiveBlur
        // sees ALPHA<1, CANDISABLEBLEND breaks) — every grid rebuild then
        // flickered dim/bright. Population is expressed by the box scale in
        // currentBox() alone.
        renderWindowLive(w, m, px, px, 1.0F, when, round, roundPow);
    }
}

// The picked-up strip window's floating box while dragging: small AND offset down-right of
// the actual cursor, for the same reason as the grid-drag box above (dragBox) — a preview
// sitting right under the pointer hides the drop-zone indicator on the card underneath it,
// making it hard to see exactly where you're about to drop the window. Hit-testing on
// release still uses the real cursor position; only the VISUAL sits offset/shrunk.
LRect Overview::dragStripBox() const {
    const auto w = m_drag.win.lock();
    if (!w)
        return LRect{0, 0, 0, 0};
    const auto   size   = w->sizeAnimation()->goal();
    const double aspect = (size.x > 0 && size.y > 0) ? size.x / size.y : 16.0 / 9.0;
    const double w_     = 150.0; // fixed on-screen preview width while dragging off the strip
    const double h_     = w_ / std::max(0.1, aspect);
    constexpr double offX = 46.0, offY = 64.0; // clear of the cursor/hotspot, down-right — same convention as dragBox()
    return LRect{m_drag.x + offX, m_drag.y + offY, w_, h_};
}

void Overview::renderDragTile() const {
    const int dragIdx = draggedTile();
    if (dragIdx >= 0) {
        drawPreviewTile(static_cast<size_t>(dragIdx), dragBox(), true); // chrome; surface queued in renderDragWindow
        return;
    }
    if (m_drag.press == Drag::Press::StripWin && !m_drag.win.expired())
        drawDragStripChrome();
}

// Chrome (shadow/border/backing) for a window being dragged straight off the strip —
// same visual language as a grid-tile drag (drawPreviewTile), scaled down to match.
void Overview::drawDragStripChrome() const {
    const auto m = m_monitor.lock();
    const auto w = m_drag.win.lock();
    if (!m || !w)
        return;
    const double s         = m->m_scale;
    const double e         = eased();
    const LRect  lb        = dragStripBox();
    const int    round     = clampRound(cfgInt("plugin:gloview:preview_round", 12), lb.w, lb.h);
    const float  roundPow  = cfgFloat("plugin:gloview:preview_round_power", 2.0F);
    const auto   shadowCol = argb(cfgColor("shadow_color", "0x70000000"), 1.0);
    const auto   hoverCol  = argb(cfgColor("hover_border", "0xf0ffffff"), e);

    // Same reasoning as drawPreviewTile's shadow above — its box is tile-sized (just offset),
    // so its solid core sits inside the tile's own footprint and now shows through real
    // transparency; cut hard rather than reshape the geometry blind.
    g_pHyprOpenGL->renderRoundedShadow(pxb(LRect{lb.x, lb.y + 14.0, lb.w, lb.h}, s), pxr(round, s), roundPow, static_cast<int>(30.0 * s), Config::CGradientValueData(shadowCol), e * 0.18);

    // Real border stroke, not a filled underlay — same reasoning as drawPreviewTile's hover
    // ring above: a filled rect would show through this window's own transparency as a solid
    // wash instead of a frame.
    const int                        th = cfgInt("plugin:gloview:hover_border_size", 3); // 0 = no ring
    const Config::CGradientValueData grad(hoverCol);
    g_pHyprOpenGL->renderBorder(pxb(lb, s), grad, {.round = pxr(round, s), .roundingPower = roundPow, .borderSize = th, .a = 1.0F, .outerRound = outerRoundPx(round, th, roundPow, s)});

    // Same as drawPreviewTile's backing above — thin near-invisible safety margin only, not
    // a deliberate/configurable tint. See its comment for the full reasoning.
    safetyBacking(lb, s, cfgColor("backing_color", "0xff14181f"), 0.08, round,
                  roundPow);
}

void Overview::renderDragWindow() const {
    const int dragIdx = draggedTile();
    const auto m = m_monitor.lock();
    if (!m)
        return;
    if (dragIdx >= 0) {
        const auto w = m_tiles[dragIdx].win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            return;
        const double e     = eased();
        const double scale = m->m_scale;
        const LRect  lb    = tileContentBox(static_cast<size_t>(dragIdx), dragBox());
        const CBox   px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
        const int    round = pxr(cfgInt("plugin:gloview:preview_round", 12), scale);
        renderWindowLive(w, m, px, px, static_cast<float>(e), Time::steadyNow(), round, cfgFloat("plugin:gloview:preview_round_power", 2.0F));
        return;
    }
    if (m_drag.press == Drag::Press::StripWin) {
        const auto w = m_drag.win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            return;
        const double e     = eased();
        const double scale = m->m_scale;
        const LRect  lb    = dragStripBox();
        const CBox   px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
        const int    round = pxr(clampRound(cfgInt("plugin:gloview:preview_round", 12), lb.w, lb.h), scale);
        renderWindowLive(w, m, px, px, static_cast<float>(e), Time::steadyNow(), round, cfgFloat("plugin:gloview:preview_round_power", 2.0F));
    }
}

} // namespace gloview
