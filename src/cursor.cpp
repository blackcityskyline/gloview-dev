// Hardware-accelerated cursor module implementation.
//
// See cursor.hpp for the full design rationale.

#include "cursor.hpp"

#include <algorithm>
#include <hyprutils/math/Region.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

void CCursorModule::onOpen(PHLMONITOR monitor, const std::string& mode) {
    resetCursorBox();
    m_softwareOnly = (mode == "software");
    (void)monitor;
}

void CCursorModule::onClose() {
    resetCursorBox();
}

bool CCursorModule::hasHardwareCursor(PHLMONITOR monitor) const {
    if (!monitor || !Pointer::mgr())
        return false;
    if (m_softwareOnly)
        return false;
    // hasVisibleHWCursor checks: no software lock, HW didn't fail, cursor
    // exists, and the renderer says the cursor should be visible.
    return Pointer::mgr()->hasVisibleHWCursor(monitor);
}

CBox CCursorModule::moveDamage(PHLMONITOR monitor) const {
    if (!monitor || !Pointer::mgr())
        return CBox{};

    const CBox cur = Pointer::mgr()->getCursorBoxGlobal();

    // On the very first move we have no prior box to erase; just damage the
    // current cursor so it re-renders.
    if (!m_haveLastBox) {
        // monitor-local, rounded to be safe
        return CBox{cur.x - monitor->m_position.x, cur.y - monitor->m_position.y,
                    cur.w, cur.h};
    }

    // Union of old + new cursor boxes (global coords) — covers both the trail
    // to erase and the new cursor to draw.
    const double x0 = std::min(m_lastBox.x, cur.x);
    const double y0 = std::min(m_lastBox.y, cur.y);
    const double x1 = std::max(m_lastBox.x + m_lastBox.w, cur.x + cur.w);
    const double y1 = std::max(m_lastBox.y + m_lastBox.h, cur.y + cur.h);
    const CBox u{x0, y0, x1 - x0, y1 - y0};

    // Convert to monitor-local for the caller.
    // damageBox is global, actually — but the original onMouseMove passed a
    // global union box directly. Keep global here.
    return u;
}

void CCursorModule::renderOnTop(PHLMONITOR monitor, const CHyprColor& backdropColor) const {
    if (!monitor || !Pointer::mgr() || !g_pHyprOpenGL)
        return;

    // --- Hardware cursor: nothing to do ---------------------------------------
    // The KMS cursor plane is composited above the framebuffer by the display
    // hardware — it's always visible over the dim backdrop, needs zero
    // framebuffer writes, and moves without any per-frame GPU work. We must NOT
    // draw a software copy here or we'd pollute the framebuffer with stale
    // pixels that can't be erased (trails), and we'd waste GPU every frame.
    if (hasHardwareCursor(monitor))
        return;

    // --- Software cursor fallback --------------------------------------------
    const auto tex = Pointer::mgr()->getCurrentCursorTexture();
    if (!tex)
        return;

    const CBox g = Pointer::mgr()->getCursorBoxGlobal();
    if (g.w <= 0 || g.h <= 0)
        return;

    // Convert to monitor-local pixel coords (renderTexture/renderRect want
    // pixels, not logical, since the projection is already set up for this
    // monitor in beginRender).
    const CBox gpl{
        (g.x - monitor->m_position.x) * monitor->m_scale,
        (g.y - monitor->m_position.y) * monitor->m_scale,
        g.w * monitor->m_scale,
        g.h * monitor->m_scale,
    };
    const CBox curPx = CBox{std::round(gpl.x), std::round(gpl.y),
                            std::round(gpl.w), std::round(gpl.h)};

    // Erase the previous cursor position with a fully OPAQUE backdrop rect.
    // The semi-transparent backdrop in renderBackdrop() can't fully erase old
    // pixels on its own (it only blends), so without this the cursor leaves a
    // dim trail. A fully-opaque version of the same color overwrites every
    // previous pixel — invisible to the user because the backdrop is nearly
    // black (0x07/0x0a/0x10) at any alpha.
    if (m_haveLastBox && m_lastBox != g) {
        const CBox lastPl{
            (m_lastBox.x - monitor->m_position.x) * monitor->m_scale,
            (m_lastBox.y - monitor->m_position.y) * monitor->m_scale,
            m_lastBox.w * monitor->m_scale,
            m_lastBox.h * monitor->m_scale,
        };
        const CBox lastPx = CBox{std::round(lastPl.x), std::round(lastPl.y),
                                 std::round(lastPl.w), std::round(lastPl.h)};
        g_pHyprOpenGL->renderRect(lastPx,
                                  CHyprColor(backdropColor.r, backdropColor.g,
                                             backdropColor.b, 1.0),
                                  {});
    }

    // Draw the cursor texture at its current position.
    g_pHyprOpenGL->renderTexture(tex, curPx, {.a = 1.0F});

    // Remember for next frame's erase.
    m_lastBox     = g;
    m_haveLastBox = true;
}

} // namespace gloview
