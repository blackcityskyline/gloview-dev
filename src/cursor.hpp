#pragma once

// Hardware-accelerated cursor module for the gloview overview overlay.
//
// Hyprland renders the pointer BEFORE RENDER_LAST_MOMENT (our pass), so the
// overview's translucent backdrop paints over it. The naive fix — redrawing
// the cursor as a texture on top — leaves trails ("шлеифы") because:
//
//   1. Only the cursor's damaged region (union of old+new boxes) is re-rendered
//      per frame, not the full monitor.
//   2. The backdrop rect is *semi-transparent* (alpha ~0.45), so re-rendering
//      it over the old cursor position only dimly blends with the stale pixels
//      instead of fully erasing them — the cursor bleeds through as a dim trail.
//
// This module eliminates the problem by preferring the kernel's **hardware
// cursor** plane whenever Hyprland is using one. A HW cursor lives on a KMS
// display plane composited *above* the framebuffer by the display hardware:
//   - always visible over the dim backdrop (no redraw needed),
//   - zero framebuffer writes (nothing to erase → no trails possible),
//   - zero GPU work for cursor movement (the plane is repositioned by KMS).
//
// When no hardware cursor is available (driver rejected it, software-locked
// by the user, or a screenshot path), we fall back to a **software cursor**:
// we draw it ourselves into the framebuffer, but first we stamp a fully
// *opaque* backdrop-colored rect over the previous cursor position so the
// semi-transparent backdrop can't leave a dimmed trail.

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

    // Called when the overview becomes active/inactive. `monitor` is the
    // monitor the overview is on (may be null for the close path).
    void onOpen(PHLMONITOR monitor, const std::string& mode = "auto");
    void onClose();

    // Render the cursor at RENDER_LAST_MOMENT.
    //
    // If a hardware cursor plane is active the function is a no-op: the KMS
    // plane handles visibility and needs no framebuffer work.
    //
    // With a software cursor, erases the previous frame's cursor with an opaque
    // backdrop-colored rect (preventing trails), then draws the cursor texture.
    //
    // Parameters:
    //   monitor       — the overview monitor
    //   backdropColor — the exact CHyprColor renderBackdrop() used (RGB + alpha),
    //                  drawn fully opaque at the erase site
    void renderOnTop(PHLMONITOR monitor, const CHyprColor& backdropColor) const;

    // Called on a mousemove event. Returns the damage region (in global coords)
    // the caller should schedule so the erase+redraw region is repainted. The
    // union of the old and new cursor boxes gives full coverage of both the trail
    // and the new cursor.
    CBox moveDamage(PHLMONITOR monitor) const;

    // True when a hardware cursor plane is currently driving the pointer for
    // this monitor — renderOnTop() becomes a no-op in that case.
    bool hasHardwareCursor(PHLMONITOR monitor) const;

    // Whether the user forced software cursor mode ("cursor_mode = software").
    bool softwareOnly() const { return m_softwareOnly; }
    void setSoftwareOnly(bool v) { m_softwareOnly = v; }

    // Reset the tracked cursor box (call on open / close / monitor change).
    void resetCursorBox() { m_haveLastBox = false; }

private:
    bool     m_softwareOnly  = false;         // true when user forced SW
    mutable CBox m_lastBox{};                // previous SW cursor box, global coords
    mutable bool m_haveLastBox = false;      // whether m_lastBox is valid
};

} // namespace gloview
