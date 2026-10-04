#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprutils/math/Box.hpp>

namespace gloview {

void renderWindowLive(const PHLWINDOW &w, const PHLMONITOR &mon,
                      const CBox &destPx, const CBox &clipPx, float alpha,
                      const Time::steady_tp &when, int roundPx = 0,
                      float roundingPower = 2.0F, bool settled = false,
                      bool contain = false);

}
