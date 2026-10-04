#pragma once

// Cursor over the overview overlay.
//
// Hyprland draws the pointer BEFORE our pass, so the translucent backdrop
// paints over it; redrawing it as a texture instead leaves trails, because only
// the damaged region is re-rendered and the semi-transparent backdrop dimly
// blends over the stale pixels instead of erasing them.
//
// So prefer the kernel's hardware cursor plane: composited above the
// framebuffer by the display hardware — always visible, zero framebuffer writes
// (nothing to erase, so no trails), zero per-move GPU work. The software
// fallback draws the cursor itself but first stamps a FULLY OPAQUE
// backdrop-colored rect over the previous position.

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprutils/math/Box.hpp>

using CBox = Hyprutils::Math::CBox;
struct CHyprColor;

namespace gloview {

class CCursorModule {
public:
    CCursorModule()  = default;
    ~CCursorModule() = default;

    // `monitor` is the overview's monitor (may be null on the close path).
    void onOpen(PHLMONITOR monitor, const std::string& mode = "auto");
    void onClose();

    // No-op when a hardware plane is driving the pointer. Otherwise erases the
    // previous position with an OPAQUE backdrop-colored rect, then draws the
    // cursor. `backdropColor` must be the exact color renderBackdrop() used.
    void renderOnTop(PHLMONITOR monitor, const CHyprColor& backdropColor) const;

    // Global-coords region the caller should schedule: the union of the old and
    // new cursor boxes.
    CBox moveDamage(PHLMONITOR monitor) const;

    // True when a hardware plane drives this monitor's pointer.
    bool hasHardwareCursor(PHLMONITOR monitor) const;

    // "cursor_mode = software" forces the fallback.
    bool softwareOnly() const { return m_softwareOnly; }
    void setSoftwareOnly(bool v) { m_softwareOnly = v; }

    // On open / close / monitor change.
    void resetCursorBox() { m_haveLastBox = false; }

private:
    bool     m_softwareOnly  = false;
    mutable CBox m_lastBox{};           // previous SW cursor box, global
    mutable bool m_haveLastBox = false;
};

} // namespace gloview
