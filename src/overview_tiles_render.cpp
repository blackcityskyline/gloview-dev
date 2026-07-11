#include "overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/Compositor.hpp>
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
#include <hyprland/src/managers/PointerManager.hpp>
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
// duplicated since both TUs need the exact same ~70-line implementation.
void renderWindowLive(const PHLWINDOW& w, const PHLMONITOR& mon, const CBox& destPx, const CBox& clipPx, float alpha, const Time::steady_tp& when, int roundPx = 0, float roundingPower = 2.0F);

namespace {

CBox box(const LRect& r) {
    return CBox{r.x, r.y, r.w, r.h};
}

// Hyprland's immediate-mode renderRect/renderTexture/renderRoundedShadow feed the box
// STRAIGHT to projectBoxToTarget, which expects transformed monitor-PIXEL coordinates and
// applies NO monitor scale itself (verified against Renderer.cpp: clipBox/scaledWindowBox are
// pre-.scale(m_scale)'d before applyToBox). All gloview chrome is authored in monitor-LOGICAL
// pixels, so it MUST be pre-scaled by mon->m_scale before drawing — otherwise on any monitor
// with scale != 1 (HiDPI / fractional like 1.2) the whole chrome renders at 1/scale size and
// top-left-biased, while the live window surfaces (renderWindowLive, which converts to pixels
// itself) land correctly → the overview looks "distorted". Chrome-only; surfaces are already
// pixel-space. Round radii / blur ranges scale too so corners/shadows keep their proportion.
CBox pxb(const CBox& b, double s) {
    return CBox{b.x * s, b.y * s, b.w * s, b.h * s};
}
CBox pxb(const LRect& r, double s) {
    return CBox{r.x * s, r.y * s, r.w * s, r.h * s};
}
int pxr(double round, double s) {
    return static_cast<int>(round * s);
}

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

CHyprColor argb(Hyprlang::INT raw, double alphaMul = 1.0) {
    const auto a = static_cast<double>((raw >> 24) & 0xFF) / 255.0;
    const auto r = static_cast<double>((raw >> 16) & 0xFF) / 255.0;
    const auto g = static_cast<double>((raw >> 8) & 0xFF) / 255.0;
    const auto b = static_cast<double>(raw & 0xFF) / 255.0;
    return CHyprColor(r, g, b, a * std::clamp(alphaMul, 0.0, 1.0));
}

// The unified preview_round (task #6) is authored against full-size grid tiles; a strip-drag
// preview is much smaller, so clamp per call site to whatever the box allows.
int clampRound(int round, double w, double h) {
    return static_cast<int>(std::clamp(static_cast<double>(round), 0.0, std::min(w, h) * 0.5));
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

// Same idea, for a strip card's "close every window on this workspace" button (task #8);
// shares the same size/position config as the per-window button (task #10) for consistency.
LRect Overview::stripCloseButtonRect(const LRect& card) const {
    return closeButtonRect(card);
}

// plugin:gloview:close_button_visibility == "always" — show close buttons on every tile
// and strip card all the time, not just in desktop mode / while Shift is the desktop-mode
// key (task #9).
bool Overview::closeButtonsAlwaysOn() const {
    return cfgStr("plugin:gloview:close_button_visibility", "shift") == "always";
}

void Overview::drawPreviewTile(size_t i, const LRect& slot, bool lift) const {
    const auto m = m_monitor.lock();
    if (!m || i >= m_tiles.size())
        return;
    const double s         = m->m_scale; // logical→pixel; Hyprland's renderRect wants pixel coords
    const double e         = eased();
    const int    round     = cfgInt("plugin:gloview:preview_round", 12);
    const float  roundPow  = cfgFloat("plugin:gloview:preview_round_power", 2.0F);
    const auto   shadowCol = argb(cfgColor("plugin:gloview:shadow_color", 0x70000000), 1.0);
    const auto   hoverCol  = argb(cfgColor("plugin:gloview:hover_border", 0xf0ffffff), e);

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
    const double range = lift ? 30.0 : 16.0;
    const double dy    = lift ? 14.0 : 6.0;
    g_pHyprOpenGL->renderRoundedShadow(pxb(LRect{lb.x, lb.y + dy, lb.w, lb.h}, s), pxr(round, s), roundPow, static_cast<int>(range * s), shadowCol, e * 0.9);

    const bool   framed   = (static_cast<int>(i) == m_hovered || lift);
    const bool   selected = (static_cast<int>(i) == m_selected) && !lift; // keyboard-nav cursor
    const double th       = std::max(1, cfgInt("plugin:gloview:hover_border_size", 3));

    // border underlay grown by the line width; the live surface on top (exactly lb) leaves a
    // clean ring. Hover ring takes precedence over the coincident keyboard selection ring.
    // roundingPower matches the content for the same reason as the shadow above.
    if (framed) {
        const CBox c = box(lb);
        g_pHyprOpenGL->renderRect(pxb(CBox(c.x - th, c.y - th, c.w + 2 * th, c.h + 2 * th), s), hoverCol, {.round = pxr(round + th, s), .roundingPower = roundPow});
    } else if (selected) {
        const auto   selCol = argb(cfgColor("plugin:gloview:select_border", 0xf066ccff), e);
        const double st     = std::max(1, cfgInt("plugin:gloview:select_border_size", 3));
        const CBox   c      = box(lb);
        g_pHyprOpenGL->renderRect(pxb(CBox(c.x - st, c.y - st, c.w + 2 * st, c.h + 2 * st), s), selCol, {.round = pxr(round + st, s), .roundingPower = roundPow});
    }

    // opaque backing under the live surface (transparent clients would leak the blurred
    // backdrop). INSET 1px: backing is a logical rect (rounded OUTWARD), surface is clipped in
    // pixel space, so on a fractional edge the backing is ~1px wider and peeks as a dark seam;
    // the inset keeps it under the over-covered surface.
    const LRect bb{lb.x + 1.0, lb.y + 1.0, std::max(0.0, lb.w - 2.0), std::max(0.0, lb.h - 2.0)};
    g_pHyprOpenGL->renderRect(pxb(bb, s), argb(cfgColor("plugin:gloview:preview_bg", 0xff14181f), 1.0), {.round = pxr(round, s), .roundingPower = roundPow});

    // window title in a dark pill below the tile (on hover or keyboard selection)
    if ((framed || selected) && !lift && t.label && t.label->m_size.x > 0) {
        const double lw   = t.label->m_size.x;
        const double lh   = t.label->m_size.y;
        const double padX = 14.0, padY = 6.0;
        const double pw   = lw + 2 * padX;
        const double ph   = lh + 2 * padY;
        const double px   = std::clamp(lb.cx() - pw / 2.0, 6.0, m->m_size.x - pw - 6.0);
        const double py   = std::min(lb.y + lb.h + 10.0, m->m_size.y - ph - 6.0);
        g_pHyprOpenGL->renderRect(pxb(CBox(px, py, pw, ph), s), argb(0xcc11151c, e), {.round = pxr(ph / 2.0, s)});
        g_pHyprOpenGL->renderTexture(t.label, pxb(CBox(px + padX, py + padY, lw, lh), s), {.a = static_cast<float>(e)});
    }
}

// Fit `slot` to the window's real aspect so the live surface fills it exactly (uniform scale).
// Used by both tile chrome and the queued surface so they coincide.
LRect Overview::tileContentBox(size_t i, const LRect& slot) const {
    // Desktop (canvas) mode: slot already carries the window's aspect (m_canvasPos froze
    // survivors). Use it AS-IS, not the live aspect, so a survivor Hyprland re-tiled to a new
    // shape keeps its frozen preview shape instead of reshaping inside the frozen slot.
    if (m_desktopMode)
        return slot;
    double aspect = slot.w / std::max(1.0, slot.h);
    if (i < m_tiles.size()) {
        if (const auto w = m_tiles[i].win.lock()) {
            const auto s = w->m_realSize->goal();
            if (s.x > 0 && s.y > 0)
                aspect = s.x / s.y;
        }
    }
    return fitInside(slot, aspect);
}

LRect Overview::dragBox() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
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
        return LRect{m_dragX - m_grabDX, m_dragY - m_grabDY, base.w, base.h};
    const double w = base.w * 0.5;
    const double h = base.h * 0.5;
    constexpr double offX = 46.0, offY = 64.0; // clear of the cursor/hotspot, down-right
    return LRect{m_dragX + offX, m_dragY + offY, w, h};
}

void Overview::renderPreviews() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
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
    if (!m_desktopMode && !closeButtonsAlwaysOn())
        return;
    const auto m = m_monitor.lock();
    if (!m)
        return;
    const double s      = m->m_scale;
    const double e      = eased();
    const int    dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == dragIdx)
            continue;
        const auto w = m_tiles[i].win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        const LRect lb = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        const LRect br = closeButtonRect(lb);
        g_pHyprOpenGL->renderRect(pxb(br, s), argb(cfgColor("plugin:gloview:close_button_color", 0xe6e23b3b), e), {.round = pxr(br.h / 2.0, s)});
        if (m_closeGlyph && m_closeGlyph->m_size.x > 0) {
            const double gw = m_closeGlyph->m_size.x, gh = m_closeGlyph->m_size.y;
            const double gs = std::min((br.w * 0.62) / std::max(1.0, gw), (br.h * 0.62) / std::max(1.0, gh));
            const double dw = gw * gs, dh = gh * gs;
            g_pHyprOpenGL->renderTexture(m_closeGlyph, pxb(CBox(br.x + (br.w - dw) / 2.0, br.y + (br.h - dh) / 2.0, dw, dh), s), {.a = static_cast<float>(e)});
        }
    }
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
    const int    dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
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
        renderWindowLive(w, m, px, px, 1.0F, when, round, roundPow);
    }
}

// The picked-up strip window's floating box while dragging: small AND offset down-right of
// the actual cursor, for the same reason as the grid-drag box above (dragBox) — a preview
// sitting right under the pointer hides the drop-zone indicator on the card underneath it,
// making it hard to see exactly where you're about to drop the window. Hit-testing on
// release still uses the real cursor position; only the VISUAL sits offset/shrunk.
LRect Overview::dragStripBox() const {
    const auto w = m_dragStripWin.lock();
    if (!w)
        return LRect{0, 0, 0, 0};
    const auto   size   = w->m_realSize->goal();
    const double aspect = (size.x > 0 && size.y > 0) ? size.x / size.y : 16.0 / 9.0;
    const double w_     = 150.0; // fixed on-screen preview width while dragging off the strip
    const double h_     = w_ / std::max(0.1, aspect);
    constexpr double offX = 46.0, offY = 64.0; // clear of the cursor/hotspot, down-right — same convention as dragBox()
    return LRect{m_dragX + offX, m_dragY + offY, w_, h_};
}

void Overview::renderDragTile() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    if (dragIdx >= 0) {
        drawPreviewTile(static_cast<size_t>(dragIdx), dragBox(), true); // chrome; surface queued in renderDragWindow
        return;
    }
    if (m_dragging && m_pressStripItem >= 0 && !m_dragStripWin.expired())
        drawDragStripChrome();
}

// Chrome (shadow/border/backing) for a window being dragged straight off the strip —
// same visual language as a grid-tile drag (drawPreviewTile), scaled down to match.
void Overview::drawDragStripChrome() const {
    const auto m = m_monitor.lock();
    const auto w = m_dragStripWin.lock();
    if (!m || !w)
        return;
    const double s         = m->m_scale;
    const double e         = eased();
    const LRect  lb        = dragStripBox();
    const int    round     = clampRound(cfgInt("plugin:gloview:preview_round", 12), lb.w, lb.h);
    const float  roundPow  = cfgFloat("plugin:gloview:preview_round_power", 2.0F);
    const auto   shadowCol = argb(cfgColor("plugin:gloview:shadow_color", 0x70000000), 1.0);
    const auto   hoverCol  = argb(cfgColor("plugin:gloview:hover_border", 0xf0ffffff), e);

    g_pHyprOpenGL->renderRoundedShadow(pxb(LRect{lb.x, lb.y + 14.0, lb.w, lb.h}, s), pxr(round, s), roundPow, static_cast<int>(30.0 * s), shadowCol, e * 0.9);

    const double th = std::max(1, cfgInt("plugin:gloview:hover_border_size", 3));
    const CBox   c  = box(lb);
    g_pHyprOpenGL->renderRect(pxb(CBox(c.x - th, c.y - th, c.w + 2 * th, c.h + 2 * th), s), hoverCol, {.round = pxr(round + th, s), .roundingPower = roundPow});

    const LRect bb{lb.x + 1.0, lb.y + 1.0, std::max(0.0, lb.w - 2.0), std::max(0.0, lb.h - 2.0)};
    g_pHyprOpenGL->renderRect(pxb(bb, s), argb(cfgColor("plugin:gloview:preview_bg", 0xff14181f), 1.0), {.round = pxr(round, s), .roundingPower = roundPow});
}

void Overview::renderDragWindow() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
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
    if (m_dragging && m_pressStripItem >= 0) {
        const auto w = m_dragStripWin.lock();
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
