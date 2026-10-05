#pragma once

// in: LOGICAL, except projectBoxToTarget: PIXEL, so pre-scale
#include <algorithm>
#include <cmath>

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/render/OpenGL.hpp>

#include "../layout.hpp"

namespace gloview {

// round(): pixman truncates the border's scissor
inline CBox pxb(const CBox &b, double s) {
  return CBox{b.x * s, b.y * s, b.w * s, b.h * s}.round();
}
inline CBox pxb(const LRect &r, double s) {
  return CBox{r.x * s, r.y * s, r.w * s, r.h * s}.round();
}
// truncate, never round: must match the shader's radius
inline int pxr(double round, double s) { return static_cast<int>(round * s); }

inline int outerRoundPx(double round, double borderSize, double roundingPower,
                        double scale) {
  const double correction =
      borderSize * (M_SQRT2 - 1.0) * std::max(2.0 - roundingPower, 0.0);
  return static_cast<int>((round + borderSize - correction) * scale);
}

inline int clampRound(int round, double w, double h) {
  return static_cast<int>(
      std::clamp(static_cast<double>(round), 0.0, std::min(w, h) * 0.5));
}

inline CHyprColor argb(Hyprlang::INT raw, double alphaMul = 1.0) {
  const auto a = static_cast<double>((raw >> 24) & 0xFF) / 255.0;
  const auto r = static_cast<double>((raw >> 16) & 0xFF) / 255.0;
  const auto g = static_cast<double>((raw >> 8) & 0xFF) / 255.0;
  const auto b = static_cast<double>(raw & 0xFF) / 255.0;
  return CHyprColor(r, g, b, a * std::clamp(alphaMul, 0.0, 1.0));
}

// STROKE, never a filled rect grown by borderSize
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

// renderWindowLive over-covers by this much so sub-pixel snapping cannot open a
// seam; the content radius follows it out.
inline constexpr float kSeamPadPx = 1.5F;

// The corner radius a tile's CONTENT must carry so its arc lands on the ring's
// outer edge, from the ring's OWN inputs. +1: Hyprland subtracts one.
// overCovers false = a contained window, whose arc does not walk outward.
inline int contentRadiusPx(int round, int width, float roundPow, double scale,
                           bool overCovers = true) {
  return pxr(round, scale) + static_cast<int>(std::lround(width * scale)) +
         (overCovers ? pxr(kSeamPadPx, scale) : 0) + 1;
}

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

// not a tint
inline void safetyBacking(const LRect &lb, double s, const CHyprColor &col,
                          double alphaMul, int round, float roundPow) {
  const LRect bb{lb.x + 1.0, lb.y + 1.0, std::max(0.0, lb.w - 2.0),
                 std::max(0.0, lb.h - 2.0)};
  // renderRect RASSERTs on non-positive boxes
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

}
