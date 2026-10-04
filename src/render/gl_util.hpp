#pragma once

// Shared GL-drawing helpers for the overlay's immediate-mode chrome (render/*
// view TUs; input/actions include it for hit-test geometry only).
//
// Chrome is authored in monitor-LOGICAL pixels, but renderRect/renderTexture/
// renderRoundedShadow feed the box straight to projectBoxToTarget, which expects
// monitor-PIXEL coords and applies no scale itself. Everything must be pre-scaled
// or the chrome renders at 1/scale while the live surfaces land correctly.
//
// The trailing .round() in pxb() is load-bearing: renderBorder's scissor region
// comes from CRegion(CBox) -> pixman_region32_init_rect, which TRUNCATES a
// fractional box. That region decides which pixels the shader runs on, so at
// 1–3px borders an off-by-one edge eats most of the ring. Every decoration that
// scales a box to pixels ends .scale(...).round() for the same reason.

#include <algorithm>
#include <cmath>

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/render/OpenGL.hpp>

#include "../layout.hpp"

namespace gloview {

inline CBox pxb(const CBox &b, double s) {
  return CBox{b.x * s, b.y * s, b.w * s, b.h * s}.round();
}
inline CBox pxb(const LRect &r, double s) {
  return CBox{r.x * s, r.y * s, r.w * s, r.h * s}.round();
}
// TRUNCATION, matching how the border decoration assigns its radius into an int
// field — never std::round. Any radius must truncate the same way or the two the
// shader receives drift apart and open a corner gap at thin border sizes.
inline int pxr(double round, double s) { return static_cast<int>(round * s); }

// Always explicit: passing -1 ("reuse round") makes the shader's solid-ring-
// interior branch unreachable, so every ring pixel goes through an antialiasing
// fade and coverage thins at the diagonal. round/borderSize are LOGICAL px,
// truncated once after scaling — same order as the real call site.
inline int outerRoundPx(double round, double borderSize, double roundingPower,
                        double scale) {
  const double correction =
      borderSize * (M_SQRT2 - 1.0) * std::max(2.0 - roundingPower, 0.0);
  return static_cast<int>((round + borderSize - correction) * scale);
}

// preview_round is authored for full-size grid tiles; small slots clamp it so the
// radius never exceeds half the slot's short side.
inline int clampRound(int round, double w, double h) {
  return static_cast<int>(
      std::clamp(static_cast<double>(round), 0.0, std::min(w, h) * 0.5));
}

// "0xAARRGGBB" literal -> CHyprColor with alpha multiplied (clamped 0..1).
inline CHyprColor argb(Hyprlang::INT raw, double alphaMul = 1.0) {
  const auto a = static_cast<double>((raw >> 24) & 0xFF) / 255.0;
  const auto r = static_cast<double>((raw >> 16) & 0xFF) / 255.0;
  const auto g = static_cast<double>((raw >> 8) & 0xFF) / 255.0;
  const auto b = static_cast<double>(raw & 0xFF) / 255.0;
  return CHyprColor(r, g, b, a * std::clamp(alphaMul, 0.0, 1.0));
}

// ---- chrome kernel ----------------------------------------------------------
// The three primitives every tile/card chrome piece is composed of. All take
// LOGICAL boxes + scale, like everything here.
//
// * Ring = renderBorder STROKE, never a filled rect grown by the line width: a
//   filled underlay only works when the surface on top is fully opaque, and
//   over a transparent window it reads as a wash over the whole preview.
//   `lb` is passed UN-grown — renderBorder expands outward by borderSize itself.
//   Sizes < 1 are an explicit "off".
// * Backing = thin near-invisible margin covering the 1-3px edge seam where the
//   rounded logical backing would peek past the pixel-clipped surface. NOT a
//   tint; INSET 1px so it stays under the over-covered surface.
// * Shadow = renderRoundedShadow on the tile-sized box shifted down dy, so the
//   solid core sits inside the footprint; the call site cuts alpha hard so it
//   reads as depth cue, not a second layer.
inline void strokeRing(const LRect &lb, double s, const CHyprColor &col,
                       int size, int round, float roundPow) {
  if (size < 1)
    return;
  Render::GL::g_pHyprOpenGL->renderBorder(
      pxb(lb, s), Config::CGradientValueData(col),
      {.round = pxr(round, s),
       .roundingPower = roundPow,
       .borderSize = size,
       .a = 1.0F,
       .outerRound = outerRoundPx(round, size, roundPow, s)});
}

// Ring around an ALREADY-PIXEL box (pulses etc.), explicit alpha.
inline void strokeRingPx(const CBox &b, const CHyprColor &col, float alpha,
                         int round, float roundPow) {
  Render::GL::g_pHyprOpenGL->renderBorder(
      b, Config::CGradientValueData(col),
      {.round = round,
       .roundingPower = roundPow,
       .borderSize = 2,
       .a = alpha,
       .outerRound = outerRoundPx(round, 2, roundPow, 1.0)});
}

inline CHyprColor argb(const CHyprColor &c, double alphaMul = 1.0) {
  return CHyprColor(c.r, c.g, c.b, c.a * std::clamp(alphaMul, 0.0, 1.0));
}

inline void safetyBacking(const LRect &lb, double s, const CHyprColor &col,
                          double alphaMul, int round, float roundPow) {
  const LRect bb{lb.x + 1.0, lb.y + 1.0, std::max(0.0, lb.w - 2.0),
                 std::max(0.0, lb.h - 2.0)};
  // The inset can reach zero, and renderRect RASSERTs on non-positive boxes. A
  // zero-area backing draws nothing anyway: skip.
  if (bb.w < 0.5 || bb.h < 0.5)
    return;
  Render::GL::g_pHyprOpenGL->renderRect(pxb(bb, s), argb(col, alphaMul),
                                        {.round = pxr(round, s),
                                         .roundingPower = roundPow});
}

inline void dropShadow(const LRect &lb, double s, const CHyprColor &col,
                       double dy, double range, float alpha, int round,
                       float roundPow) {
  Render::GL::g_pHyprOpenGL->renderRoundedShadow(
      pxb(LRect{lb.x, lb.y + dy, lb.w, lb.h}, s), pxr(round, s), roundPow,
      static_cast<int>(range * s), Config::CGradientValueData(col), alpha);
}

} // namespace gloview
