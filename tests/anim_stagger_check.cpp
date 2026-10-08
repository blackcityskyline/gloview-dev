// The entry ladder, as pure arithmetic.
//
// Every tile that is not the selected one is delayed a little further into the entry
// clock, so a workspace arrives as a staircase instead of everything landing at once.
// staggered() in clocks.cpp reads the spread from anim::param() and cannot be linked by a
// test, so the arithmetic under it is here with the spread passed in.
//
// The two things that have actually gone wrong with this function, both silent:
//
//   - spread is not the only knob. Stagger is a ceiling and StaggerPerTile scales with
//     the tile count, so the effective spread is min of the two. A test that used only
//     the ceiling would pass while the product ran past it.
//   - with spread == 1 the denominator vanishes and the last tile resolves to NaN, which
//     propagates into an LRect and then into a box the compositor draws. The floor away
//     from zero is not defensive noise; it is what keeps 1 - spread off the divide.

#include <cassert>
#include <cmath>
#include <cstdio>

#include "../src/anim_types.hpp"

using gloview::anim::stagger;

namespace {

constexpr double kEps = 1e-12;
// The shipped defaults: Stagger 0.08, StaggerPerTile 0.015.
constexpr double kStagger = 0.08;
constexpr double kPerTile = 0.015;

double spreadFor(int n) {
  return std::min(kStagger, kPerTile * n);
}

} // namespace

int main() {
  // --- the trivial case, which is the common one -----------------------------------
  // A one-tile workspace takes this on every switch: there is nothing to stagger
  // against, so the tile gets the clock unshifted.
  for (const double b : {0.0, 0.5, 1.0}) {
    assert(std::abs(stagger(b, 0, 1, kStagger) - b) < kEps);
    assert(std::abs(stagger(b, 0, 0, kStagger) - b) < kEps);
  }
  std::puts("n <= 1 returns the clock untouched");

  // --- the ladder falls with tile index --------------------------------------------
  // The start offset GROWS with i, so a later tile has LESS of the clock behind it: the
  // first tile leads and the last trails. Read the wrong way round this test would pass
  // with the staircase running backwards, and every workspace would arrive last-window
  // first -- which reads as a glitch, not as an error.
  for (int n = 2; n <= 8; ++n) {
    double prev = 2.0;
    for (int i = 0; i < n; ++i) {
      const double v = stagger(0.5, i, n, spreadFor(n));
      assert(v <= prev + kEps);
      assert(v >= 0.0 && v <= 1.0);
      prev = v;
    }
    // ...and the spread is strictly negative for n > 1, i.e. it is a ladder at all.
    assert(stagger(0.5, 0, n, spreadFor(n)) > stagger(0.5, n - 1, n, spreadFor(n)));
  }
  std::puts("the ladder falls with tile index, and is a ladder at all");

  // --- the first tile is not delayed -----------------------------------------------
  // i = 0 has start = 0, so it is the clock itself, only rescaled by the denominator.
  {
    const int n = 5;
    const double s = spreadFor(n);
    // start = 0 for i = 0, so the tile keeps the whole clock, stretched by 1/(1-spread).
    // base 1 clamps down to 1: the stretch can only ever be undone by the clamp, which is
    // why the last tile still lands.
    assert(std::abs(stagger(0.5, 0, n, s) - 0.5 / (1.0 - s)) < kEps);
    assert(std::abs(stagger(1.0, 0, n, s) - 1.0) < kEps);
  }
  std::puts("tile 0 runs on the clock, stretched by 1/(1-spread) and clamped");

  // --- every tile still finishes -----------------------------------------------------
  // At base = 1 the staircase must have completed: a tile that never reaches 1 leaves a
  // card at 85% scale forever, which is what appear = 1.0 is supposed to prevent.
  for (int n = 2; n <= 8; ++n) {
    for (int i = 0; i < n; ++i) {
      const double v = stagger(1.0, i, n, spreadFor(n));
      assert(v > 0.999);
    }
  }
  std::puts("every tile completes: base 1 resolves to 1");

  // --- the ceiling holds -------------------------------------------------------------
  // Per-tile * n would exceed the ceiling at n > 5 with the shipped defaults. min() keeps
  // it at 0.08; the arithmetic must not depend on the caller having done that.
  {
    const double s = spreadFor(50);
    assert(std::abs(s - kStagger) < kEps);
  }
  std::puts("the effective spread never exceeds the ceiling");

  // --- spread == 1 does not divide by zero ------------------------------------------
  // The degenerate knob: a user setting appear_stagger = 1. 1 - spread is 0, and without
  // the floor the last tile is 0/0.
  for (int i = 0; i < 5; ++i) {
    const double v = stagger(0.5, i, 5, 1.0);
    assert(std::isfinite(v));
    assert(v >= 0.0 && v <= 1.0);
  }
  std::puts("spread 1 stays finite instead of NaN");

  // --- out-of-range indices do not escape [0,1] --------------------------------------
  // indices come from a tile loop, but a clamp here is what keeps a wrong index from
  // producing a box outside the monitor.
  for (const int i : {-1, -100, 500}) {
    const double v = stagger(0.5, i, 5, spreadFor(5));
    assert(std::isfinite(v));
    assert(v >= 0.0 && v <= 1.0);
  }
  std::puts("a bad tile index clamps instead of escaping");

  // --- nonsense spread is not trusted --------------------------------------------------
  assert(std::isfinite(stagger(0.5, 2, 5, -1.0)));
  assert(std::isfinite(stagger(0.5, 2, 5, 99.0)));
  assert(std::abs(stagger(0.5, 2, 5, 0.0) - 0.5) < kEps); // no stagger is the clock
  std::puts("nonsense spread cannot produce NaN");

  std::puts("OK");
  return 0;
}