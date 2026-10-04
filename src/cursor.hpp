#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprutils/math/Box.hpp>

using CBox = Hyprutils::Math::CBox;
struct CHyprColor;

namespace gloview {

// HW plane: above framebuffer, no trails
class CCursorModule {
public:
    CCursorModule()  = default;
    ~CCursorModule() = default;

    void onOpen(PHLMONITOR monitor, const std::string& mode = "auto");
    void onClose();

    // opaque erase, renderBackdrop()'s color
    void renderOnTop(PHLMONITOR monitor, const CHyprColor& backdropColor) const;

    CBox moveDamage(PHLMONITOR monitor) const;

    bool hasHardwareCursor(PHLMONITOR monitor) const;

    bool softwareOnly() const { return m_softwareOnly; }
    void setSoftwareOnly(bool v) { m_softwareOnly = v; }

    void resetCursorBox() { m_haveLastBox = false; }

private:
    bool     m_softwareOnly  = false;
    mutable CBox m_lastBox{};
    mutable bool m_haveLastBox = false;
};

}
