#pragma once

// The immediate window-content leaf (window_content.cpp). Shared by the render
// view TUs; defaults here keep call sites terse.

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprutils/math/Box.hpp>

namespace gloview {

// How a window's real shape maps onto its destination box.
enum class WindowFit {
  Cover,   // fill the box, cropping (grid tiles)
  Contain, // fit the window whole, letterboxed and centered (carousel cards)
};

// Draws the LIVE surface tree (or the frozen snapshot, when preview_mode ==
// "snapshot") into destPx, clipped to clipPx — monitor PIXEL coords.
// roundPx/roundingPower match the surrounding chrome's corners. `settled`:
// destPx is identical to last frame (no glide/drag/flight), enabling pad=0 for
// a pixel-exact corner match against the border ring. Call from pass EXECUTION
// (inside the painter), where currentFB/projection are live.
void renderWindowLive(const PHLWINDOW &w, const PHLMONITOR &mon,
                      const CBox &destPx, const CBox &clipPx, float alpha,
                      const Time::steady_tp &when, int roundPx = 0,
                      float roundingPower = 2.0F, bool settled = false,
                      WindowFit fit = WindowFit::Cover);

} // namespace gloview
