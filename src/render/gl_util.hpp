#pragma once

// Shared GL-drawing helpers for the overview's immediate-mode chrome
// (render/* view TUs; input/actions include it for hit-test geometry only).
//
// All chrome is authored in monitor-LOGICAL pixels, but Hyprland's
// immediate-mode renderRect/renderTexture/renderRoundedShadow feed the box
// STRAIGHT to projectBoxToTarget, which expects transformed monitor-PIXEL
// coordinates and applies NO monitor scale itself (verified against
// Renderer.cpp: clipBox/scaledWindowBox are pre-.scale(m_scale)'d before
// applyToBox). Everything must therefore be pre-scaled by mon->m_scale here,
// otherwise on any monitor with scale != 1 the chrome renders at 1/scale size
// and top-left-biased while the live window surfaces (window_content.cpp,
// which converts to pixels itself) land correctly — the overview looks
// "distorted".
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
// Rounds the SAME way outerRoundPx does (std::lround, not truncation).
// A truncating pxr() here previously disagreed with outerRoundPx's lround by
// up to 1px whenever `round * s` wasn't a whole number (any non-integer
// monitor scale) — the border's inner radius and outer radius were then
// computed from two DIFFERENT rounding conventions. At thick borders (2-3px)
// the discrepancy was inside the antialiasing fringe and invisible; at 1px
// it was exactly enough to open a visible gap in the corner arc (the "seam"
// where the rounded corner meets the straight edge).
inline int pxr(double round, double s) { return static_cast<int>(std::lround(round * s)); }

// renderBorder's outerRound field controls SHADER_RADIUS_OUTER. Passing -1
// tells the shader to reuse the SAME `round` variable it already computed
// internally (data.round + scaledBorderSize) for BOTH the inner and outer
// arc — self-consistent by construction, since it's literally one number
// used twice.
//
// Passing an EXPLICIT outerRound (as this function used to always do)
// instead gives the shader a SEPARATELY-computed value. For a genuine
// squircle correction (roundingPower < 2) that's necessary and matches
// CHyprBorderDecoration::draw(). But when roundingPower >= 2 the correction
// term is exactly 0 — in that case our own value and the shader's internal
// round+scaledBorderSize approximate the SAME target through two INDEPENDENT
// lround() calls on different expressions, which can differ by 1 unit on
// any non-integer monitor scale. At thick borders that 1px sat inside the
// antialiasing fringe; at 1px borders it was exactly enough to open a
// visible gap in the corner arc (the two arcs no longer share an origin).
//
// Fix: return -1 (Hyprland's own fallback) whenever no correction is
// needed, so inner and outer arcs are provably the same number. Only
// compute a distinct value for the true squircle case.
inline int outerRoundPx(double round, double borderSize, double roundingPower,
                        double scale) {
  if (roundingPower >= 2.0)
    return -1; // let the shader reuse its own round+scaledBorderSize
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
// alphaMul (clamped to 0..1). The CHyprColor overload alpha-multiplies an
// already-parsed color (config.cpp Color handles).
inline CHyprColor argb(Hyprlang::INT raw, double alphaMul = 1.0) {
  const auto a = static_cast<double>((raw >> 24) & 0xFF) / 255.0;
  const auto r = static_cast<double>((raw >> 16) & 0xFF) / 255.0;
  const auto g = static_cast<double>((raw >> 8) & 0xFF) / 255.0;
  const auto b = static_cast<double>(raw & 0xFF) / 255.0;
  return CHyprColor(r, g, b, a * std::clamp(alphaMul, 0.0, 1.0));
}

// ---- chrome kernel ----------------------------------------------------------
// The three primitives every piece of tile/card chrome is composed of. All of
// them take LOGICAL boxes + scale, like everything in this header.
//
// WHY each looks the way it does (verified once, applies to every call site):
// * Ring = renderBorder STROKE, never a filled rect grown by the line width:
//   a filled underlay relied on the live surface on top being fully opaque to
//   hide everything but the outward edge; against a transparent window it
//   reads as a solid wash over the whole preview instead of a frame.
//   renderBorder shades only its own pixels (it subtracts the inner box), so
//   transparency is safe at any size. `lb` is passed UN-grown — renderBorder
//   expands outward by borderSize itself, same as real window borders. Sizes
//   < 1 are an explicit "off".
// * Backing = thin near-invisible safety margin (backing_color, renderer
//   applies ~0.08) ONLY to cover the 1-3px edge-seam case where the rounded
//   logical backing would peek past the pixel-clipped live surface — NOT a
//   decorative tint; INSET 1px so it stays under the over-covered surface.
// * Shadow = renderRoundedShadow with the box TILE-SIZED, just shifted down
//   dy: its solid core therefore sits inside the tile footprint and shows
//   through genuinely transparent windows; alpha is cut hard (×~0.18 at the
//   call site) so it reads as depth cue, not a second layer.

inline void strokeRing(const LRect &lb, double s, const CHyprColor &col,
                       int size, int round, float roundPow) {
  if (size < 1)
    return; // renderBorder no-ops below 1 anyway; keep call sites honest
  Render::GL::g_pHyprOpenGL->renderBorder(
      pxb(lb, s), Config::CGradientValueData(col),
      {.round = pxr(round, s),
       .roundingPower = roundPow,
       .borderSize = size,
       .a = 1.0F,
       .outerRound = outerRoundPx(round, size, roundPow, s)});
}

// Ring around an ALREADY-PIXEL box (pulse flashes etc.), explicit alpha.
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
  // The inset can reach ZERO (a 2px-wide slot, a degenerate card) — and
  // renderRect RASSERTs on non-positive boxes ("width/height < 0" fires for
  // 0 too). A zero-area backing draws nothing anyway: skip.
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
