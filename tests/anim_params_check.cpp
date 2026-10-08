// The shape-parameter table, headless.
//
// Every other table in the plugin has a test that its columns line up. This one did not,
// because it lived in anim.cpp next to the config handles, which no test can link -- and
// its failure mode is the quiet kind. A Param added to the enum without a row in
// kParamKeys and kParamDefaults compiles cleanly, then reads the NEXT slot's value at
// runtime. No error, no warning, one number quietly wrong in a transition.
//
// The arrays are now sized by the enum, so a mismatch is a compile error in the plugin;
// this file is the second half of that: it says what the sizes mean, and it pins the
// values themselves because a default that moves is a visual change nobody asked for.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/anim_types.hpp"

using namespace gloview::anim;

namespace {

// A key must be a plugin option name: lower snake, no spaces. A typo here is a config key
// that exists for nobody.
void checkKeyShape(const char *key) {
  assert(key && *key);
  for (const char *p = key; *p; ++p) {
    assert((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_');
  }
  assert(std::strlen(key) >= 3);
}

} // namespace

int main() {
  // --- the table is consistent with the enum --------------------------------------
  // Sizing by the enum makes a missing row a compile error rather than an out-of-bounds
  // read; these asserts say which table is wrong when that happens.
  assert(std::size(kParamKeys) == kParamCount);
  assert(std::size(kParamDefaults) == kParamCount);
  std::puts("key and default tables match the enum");

  // --- every row is well-formed and distinct ---------------------------------------
  for (std::size_t i = 0; i < kParamCount; ++i) {
    checkKeyShape(kParamKeys[i]);
    for (std::size_t j = i + 1; j < kParamCount; ++j) {
      assert(std::strcmp(kParamKeys[i], kParamKeys[j]) != 0);
    }
  }
  std::puts("keys are well-formed and unique");

  // --- defaults are finite and sane ------------------------------------------------
  for (std::size_t i = 0; i < kParamCount; ++i) {
    const double d = kParamDefaults[i];
    assert(std::isfinite(d));
    assert(d >= 0.0);
  }
  std::puts("defaults are finite and non-negative");

  // --- the values themselves --------------------------------------------------------
  // Named, because "index 6" tells nobody what a regression means. These are the numbers
  // the render code behaves as if they were hardcoded, several of which were literals in
  // the render path until they were moved here.
  struct Case { Param p; double want; const char *what; };
  const Case cases[] = {
      {Param::Stagger,        0.08,  "entry ladder spread, in units of the entry clock"},
      {Param::StaggerPerTile, 0.015, "extra spread per tile, capped by Stagger"},
      {Param::Collapse,       0.5,   "fraction of the carousel exit spent shrinking"},
      {Param::PulseScale,     0.10,  "how far past the tile edge the ring reaches"},
      {Param::LiftScale,      0.07,  "how much a lifted tile grows"},
      {Param::LiftRise,       5.0,   "px a lifted tile rises"},
      {Param::PopScale,       0.15,  "how small a tile is at the start of its scale-in"},
      {Param::ShadowOffset,   16.0,  "tile shadow offset at rest, px"},
      {Param::LiftShadow,     30.0,  "tile shadow offset while lifted, px"},
  };
  for (const auto &c : cases) {
    const auto i = static_cast<std::size_t>(c.p);
    assert(std::abs(kParamDefaults[i] - c.want) < 1e-12);
    // The lifted shadow must be at least as far as the resting one, or dragging the tile
    // would pull its shadow under it.
    assert(std::strcmp(kParamKeys[i], "lift_shadow_offset") != 0 ||
           kParamDefaults[i] >= kParamDefaults[static_cast<std::size_t>(Param::ShadowOffset)]);
  }
  std::puts("defaults hold their documented values");

  // --- the resolution rule ------------------------------------------------------------
  // The whole of resolveParam, and the only part that could differ: an unset key falls
  // back rather than reading as zero, because zero here means "no stagger", "no rise",
  // "collapse instantly" -- each of which is a plausible config and none of them the
  // intended reading of "user did not touch this".
  assert(std::abs(resolveParam(false, 999.0, 0.08) - 0.08) < 1e-12);
  assert(std::abs(resolveParam(true, 0.5, 0.08) - 0.5) < 1e-12);
  assert(std::abs(resolveParam(true, 0.0, 0.08) - 0.0) < 1e-12); // an explicit zero is honoured
  std::puts("unset falls back, an explicit zero is kept");

  std::puts("OK");
  return 0;
}