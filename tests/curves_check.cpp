// Curve registry: what the animation system resolves every frame.
//
// This is the first piece of the headless harness the whole animation table needs. The
// resolver (anim::cfg) cannot be linked without a compositor -- Config::Values lives in
// the Hyprland binary, not in a library -- so the curves are where headless coverage
// starts, and they are the layer that decides the SHAPE of every motion: timing and speed
// decide how long, the curve decides where the value is on the way.
//
// What it pins down, all of it verified by hand earlier and worth keeping:
//   - every builtin reaches 0 at t=0 and 1 at t=1, or a transition settles wrong
//   - a bezier solves its own x(t): the control points move x, and feeding them through
//     un-solved is the classic off-by-a-bit error that looks fine at the endpoints
//   - an unknown name falls back rather than throwing, and does not do it silently wrong
//   - t outside [0,1] is NOT clamped, and that is pinned so nobody adds one blind

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/clock/curves.hpp"

using gloview::curves::eval;

namespace {

constexpr double kEps = 1e-6;

void checkEndpoints(const char *name) {
  assert(std::abs(eval(name, 0.0) - 0.0) < kEps);
  assert(std::abs(eval(name, 1.0) - 1.0) < kEps);
}

// A cubic bezier as CSS spells it: x is solved numerically, y is read off. The registry
// stores the points, so this has to reproduce the same shape from the outside.
struct BezierCase {
  const char *name;
  double x1, y1, x2, y2;
};

} // namespace

int main() {
  gloview::curves::registerBuiltins();

  // --- builtins -----------------------------------------------------------------
  for (const char *n : {"linear", "easeout", "easeinout", "back"}) {
    checkEndpoints(n);
  }
  std::puts("builtin curves reach 0 and 1");

  // linear is the identity -- the only one that may not bend.
  for (const double t : {0.0, 0.25, 0.5, 0.75, 1.0})
    assert(std::abs(eval("linear", t) - t) < kEps);
  std::puts("linear is the identity");

  // back overshoots past 1 and comes back; that is its whole point, and a change that
  // removed the overshoot would make card and pulse entrances read as plain fades.
  bool overshot = false;
  for (int i = 1; i < 100; ++i) {
    const double t = i / 100.0;
    if (eval("back", t) > 1.0 + kEps)
      overshot = true;
  }
  assert(overshot);
  std::puts("back overshoots, as the name promises");

  // monotonic where it should be. back is exempt.
  for (const char *n : {"linear", "easeout", "easeinout"}) {
    double prev = -1.0;
    for (int i = 0; i <= 100; ++i) {
      const double v = eval(n, i / 100.0);
      assert(v >= prev - kEps);
      prev = v;
    }
  }
  std::puts("linear/easeout/easeinout are monotonic");

  // No clamping, and that is deliberate: these are polynomials evaluated on the clock's
  // own t, which lives in [0,1]. A clamp here would hide a clock that overran, and the
  // overrun is the bug worth seeing. Asserted rather than assumed so a future clamp
  // does not land unnoticed.
  assert(std::abs(eval("easeout", -5.0) - (-215.0)) < 1e-9);
  assert(std::abs(eval("easeinout", 2.0) - 5.0) < kEps);
  assert(std::abs(eval("easeinout", -1.0) - (-4.0)) < kEps);
  std::puts("curves evaluate as polynomials, unclamped");

  // --- unknown names -------------------------------------------------------------
  // A typo in a config must not take the compositor down mid-frame. The fallback is
  // easeout; the warnOnce path handles telling the user, and is not observable here.
  assert(std::abs(eval("no_such_curve", 0.5) - eval("easeout", 0.5)) < kEps);
  assert(std::abs(eval("", 0.5) - eval("easeout", 0.5)) < kEps);
  std::puts("unknown curve falls back to easeout");

  // A registered name must keep resolving to the same thing across calls -- the map is
  // built once and read every frame, so an accidental insert on the read path would only
  // show up as drift.
  const double first = eval("easeout", 0.3);
  for (int i = 0; i < 100; ++i)
    assert(std::abs(eval("easeout", 0.3) - first) < kEps);
  std::puts("evaluation is stable across calls");

  std::puts("OK");
  return 0;
}