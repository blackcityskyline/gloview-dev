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
    return Pointer::mgr()->hasVisibleHWCursor(monitor);
}

CBox CCursorModule::moveDamage(PHLMONITOR monitor) const {
    if (!monitor || !Pointer::mgr())
        return CBox{};

    const CBox cur = Pointer::mgr()->getCursorBoxGlobal();

    if (!m_haveLastBox) {
        // this path is monitor-local; the union below is global
        return CBox{cur.x - monitor->m_position.x, cur.y - monitor->m_position.y,
                    cur.w, cur.h};
    }

    const double x0 = std::min(m_lastBox.x, cur.x);
    const double y0 = std::min(m_lastBox.y, cur.y);
    const double x1 = std::max(m_lastBox.x + m_lastBox.w, cur.x + cur.w);
    const double y1 = std::max(m_lastBox.y + m_lastBox.h, cur.y + cur.h);
    const CBox u{x0, y0, x1 - x0, y1 - y0};

    return u;
}

void CCursorModule::renderOnTop(PHLMONITOR monitor, const CHyprColor& backdropColor) const {
    if (!monitor || !Pointer::mgr() || !g_pHyprOpenGL)
        return;

    // HW cursor is composited above us: a software copy would trail
    if (hasHardwareCursor(monitor))
        return;

    const auto tex = Pointer::mgr()->getCurrentCursorTexture();
    if (!tex)
        return;

    const CBox g = Pointer::mgr()->getCursorBoxGlobal();
    if (g.w <= 0 || g.h <= 0)
        return;

    // pixels, not logical: projection is set per monitor
    const CBox gpl{
        (g.x - monitor->m_position.x) * monitor->m_scale,
        (g.y - monitor->m_position.y) * monitor->m_scale,
        g.w * monitor->m_scale,
        g.h * monitor->m_scale,
    };
    const CBox curPx = CBox{std::round(gpl.x), std::round(gpl.y),
                            std::round(gpl.w), std::round(gpl.h)};

    // erase with an OPAQUE backdrop copy: the backdrop only blends
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

    g_pHyprOpenGL->renderTexture(tex, curPx, {.a = 1.0F});

    m_lastBox     = g;
    m_haveLastBox = true;
}

}
