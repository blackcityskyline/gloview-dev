#pragma once

// Shared GL-drawing helpers for the overview's immediate-mode chrome
// (used by overview_render.cpp and overview_tiles_render.cpp).
//
// All chrome is authored in monitor-LOGICAL pixels, but Hyprland's
// immediate-mode renderRect/renderTexture/renderRoundedShadow feed the box
// STRAIGHT to projectBoxToTarget, which expects transformed monitor-PIXEL
// coordinates and applies NO monitor scale itself (verified against
// Renderer.cpp: clipBox/scaledWindowBox are pre-.scale(m_scale)'d before
// applyToBox). Everything must therefore be pre-scaled by mon->m_scale here,
// otherwise on any monitor with scale != 1 the chrome renders at 1/scale size
// and top-left-biased while the live window surfaces (renderWindowLive, which
// converts to pixels itself) land correctly — the overview looks "distorted".
//
// The trailing .round() in pxb() is NOT cosmetic:
// CHyprOpenGLImpl::renderBorder() builds its scissor-culling CRegion from the
// box via hyprutils' CRegion(const CBox&) ->
// pixman_region32_init_rect(int,int,uint,uint), which silently TRUNCATES a
// fractional box on the implicit double->int conversion. The ring shape itself
// comes from float shader math, but that region decides WHICH pixels the
// shader even runs on: at border_size 1–3px an off-by-one edge can eat most
// or all of the ring, each edge/corner off by its own leftover fraction (why
// missing sides aren't consistent between thicknesses). .round() snaps x/y AND
// recomputes w/h from the rounded RIGHT/BOTTOM edges, making the box exactly
// representable on the integer grid the CRegion math assumes — same reason
// every Hyprland decoration that scales a box to pixels (border/shadow/glow/
// group-bar) ends `.scale(...).round()`, never a bare `.scale()`.

#include <algorithm>
#include <cmath>

#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/Color.hpp>

#include "layout.hpp"

namespace gloview {

inline CBox pxb(const CBox &b, double s) {
  return CBox{b.x * s, b.y * s, b.w * s, b.h * s}.round();
}
inline CBox pxb(const LRect &r, double s) {
  return CBox{r.x * s, r.y * s, r.w * s, r.h * s}.round();
}
inline int pxr(double round, double s) { return static_cast<int>(round * s); }

// renderBorder's own outerRound == -1 fallback (round + scaledBorderSize, no
// correction) is what caused thin borders to show gaps near corners: real
// Hyprland window borders NEVER rely on that fallback, they always compute
// this explicitly (CHyprBorderDecoration::draw()) with a squircle-power
// correction term. round/borderSize are LOGICAL (unscaled) px, matching that
// source; the whole expression is scaled once at the end, same as there.
inline int outerRoundPx(double round, double borderSize, double roundingPower,
                        double scale) {
  const double correction =
      borderSize * (M_SQRT2 - 1.0) * std::max(2.0 - roundingPower, 0.0);
  return static_cast<int>(
      std::lround((round + borderSize - correction) * scale));
}

// preview_round is authored against full-size grid tiles; much smaller slots
// (strip cards, drag previews) clamp it per call site so the radius never
// exceeds the slot's half-size and look broken.
inline int clampRound(int round, double w, double h) {
  return static_cast<int>(
      std::clamp(static_cast<double>(round), 0.0, std::min(w, h) * 0.5));
}

// "0xAARRGGBB"-style config literal -> CHyprColor, alpha multiplied by
// alphaMul (clamped to 0..1).
inline CHyprColor argb(Hyprlang::INT raw, double alphaMul = 1.0) {
  const auto a = static_cast<double>((raw >> 24) & 0xFF) / 255.0;
  const auto r = static_cast<double>((raw >> 16) & 0xFF) / 255.0;
  const auto g = static_cast<double>((raw >> 8) & 0xFF) / 255.0;
  const auto b = static_cast<double>(raw & 0xFF) / 255.0;
  return CHyprColor(r, g, b, a * std::clamp(alphaMul, 0.0, 1.0));
}

} // namespace gloview
