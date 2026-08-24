#pragma once

// The immediate window-content leaf (see window_content.cpp). Shared by the
// render view TUs; the declaration carries the defaults so call sites stay
// terse.

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprutils/math/Box.hpp>

namespace gloview {

// Draw a window's LIVE surface tree (or its frozen snapshot texture, when
// preview_mode == "snapshot") scaled into `destPx`, clipped to `clipPx` —
// both monitor PIXEL coords. `roundPx`/`roundingPower` let the caller match
// the surrounding chrome's corner rounding. Must be called from pass
// EXECUTION (inside the painter), where currentFB/projection are live.
void renderWindowLive(const PHLWINDOW &w, const PHLMONITOR &mon,
                      const CBox &destPx, const CBox &clipPx, float alpha,
                      const Time::steady_tp &when, int roundPx = 0,
                      float roundingPower = 2.0F);

} // namespace gloview
