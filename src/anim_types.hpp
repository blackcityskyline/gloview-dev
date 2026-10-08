#pragma once

// The animation registry's VALUE TYPES, and nothing else.
//
// Deliberately free of any hyprland/ include. Config::Values and PluginAPI live in the
// compositor binary and there is no libhyprland, so a header that pulls them in cannot be
// reached from a test -- and the whole point of splitting resolve() out of anim.cpp was
// that the precedence chain could be tested without a compositor. Keeping the types here
// is what makes src/anim_resolve.cpp linkable on its own.
//
// If something needs to change in this file, it is a type, not a value: no handles, no
// globals, no functions that read state.

#include <cstddef>
#include <string_view>
#include <cstdint>
#include <optional>
#include <string>

namespace gloview::anim {

// Resolved configuration of one registered animation.
struct Cfg {
  bool        on = false;
  double      ms = 0.0;
  std::string curve;
  std::string style;
};

// Imperative override layered over the registered keys (gloview.animation({...})).
struct Rule {
  std::optional<bool>        enabled;
  std::optional<double>      speed;
  std::optional<int>         ms;
  std::optional<std::string> curve;
  std::optional<std::string> style;
};

// The values cfg() reads, lifted out of the config handles.
//
// Config::Values lives in the Hyprland binary and there is no libhyprland, so a test
// cannot link anim.cpp at all. resolve() takes these instead and touches nothing but
// plain C++ -- which is what makes the whole precedence chain testable headless.
struct Inputs {
  bool        globalEnabled = true;
  bool        enabled = true;         // the leaf's own _enabled
  bool        hasMs = false;          // _ms was set, vs "not set, use the table base"
  int         ms = 0;
  double      speed = 1.0;
  std::string curve;                  // _curve, empty when unset
  const Rule *rule = nullptr;         // gloview.animation{...}, may be null
};

// Resolve one leaf: precedence is Lua rule > config key > table default, and an
// animation switched off resolves to 1ms rather than keeping its duration -- a clock
// still has to tick, it just must not move anything.
[[nodiscard]] Cfg resolve(const Inputs &in, int baseMs, const char *curveDefault);

// SHAPE parameter resolution, same split as resolve() and for the same reason.
//
// A Param reads one float out of a config handle and falls back to the table default.
// The interesting part is not the read, it is that there are N of them, the table is
// indexed by the enum, and nothing has ever checked that the two agree -- a row added to
// the enum without a key compiles fine and silently reads the wrong slot's value.
//
// hasValue is what the handle said: registered-but-unset falls back to the default,
// which is the behaviour every other key in the plugin has.
[[nodiscard]] double resolveParam(bool hasValue, double value, double fallback);

// SHAPE parameters: not how long an animation runs but how it is shaped. They belong to an
// animation and live beside it here, rather than as literals in the render code that
// consumes them. Resolved live, exactly like the rest of the registry.
enum class Param : std::uint8_t {
  Stagger,        // entry ladder: spread of the tiles over the window
  StaggerPerTile, // ... per extra tile, capped by Stagger
  Collapse,       // carousel: fraction of the exit spent shrinking the unfocused cards
  PulseScale,     // pulse ring: how far past the tile edge it reaches
  LiftScale,      // lift: how much the picked-up tile grows
  LiftRise,       // lift: how far it rises, px
  PopScale,        // pop: how much a tile is SMALLER at the start of its scale-in
  ShadowOffset,    // tile shadow offset at rest, px
  LiftShadow,      // tile shadow offset while lifted, px
  Count
};

// The key and default tables, beside the enum they are indexed by.
//
// These lived in anim.cpp, which no test could reach, so nothing had ever checked that
// they still match the enum. A Param added without a key row compiles cleanly and then
// reads the NEXT slot's value at runtime -- a wrong number with no error anywhere. Both
// arrays are sized by the enum rather than counted, so a mismatch is a compile error in
// the plugin and an assertion in the test.
inline constexpr std::size_t kParamCount = static_cast<std::size_t>(Param::Count);

inline constexpr const char *const kParamKeys[kParamCount] = {
    "appear_stagger", "appear_stagger_per_tile", "carousel_step_collapse",
    "pulse_ring_scale", "lift_scale", "lift_rise",
    // The last three were literals in the render code: the scale-in depth, and the tile
    // shadow offset at rest and while lifted. Shape, not timing. The shadow RADIUS was
    // already configurable (shadow_radius / shadow_radius_lift); the offset was not.
    "appear_pop_scale", "shadow_offset", "lift_shadow_offset"};

inline constexpr double kParamDefaults[kParamCount] = {0.08, 0.015, 0.5, 0.10,
                                                       0.07, 5.0, 0.15, 16.0, 30.0};

// The style names a motion can be given, in one place.
//
// These were five string comparisons spread across clocks.cpp and actions.cpp, and the
// set of legal values existed only as those comparisons: adding a style meant writing a
// branch, and forgetting one left a value accepted by the config and silently ignored by
// the motion that read it. A test cannot check a set it cannot see, so it is declared.
//
// Enter styles (ws_enter_style / ws_exit_style) drive the workspace slide.
inline constexpr const char *const kEnterStyles[] = {"slide", "slidevert", "fade", "pop"};

// Swap styles (grid_swap_style / strip_swap_style) drive the exchange flight, and mirror
// model::SwapStyle one-for-one. jump_style is separate: it adds "instant".
inline constexpr const char *const kSwapStyles[] = {"horizontal", "slidevert", "fade",
                                                    "pop"};

[[nodiscard]] constexpr bool isEnterStyle(std::string_view s) {
  for (const char *k : kEnterStyles)
    if (s == k)
      return true;
  return false;
}

[[nodiscard]] constexpr bool isSwapStyle(std::string_view s) {
  for (const char *k : kSwapStyles)
    if (s == k)
      return true;
  return false;
}

// The entry stagger, as pure arithmetic.
//
// staggered() in clocks.cpp reads the spread from anim::param() and is therefore not
// reachable from a test; this is the part under test, with the spread handed in. spread
// is capped at 1 inside, so a caller passing 5 gets a staircase, not a negative
// denominator -- see the test.
//
// n <= 1 returns base: a single tile cannot be staggered against anything, and the
// staggered tile is exempt from the ladder by construction (clocks.cpp), so this is the
// path every one-tile workspace takes on every switch.
[[nodiscard]] double stagger(double base, int i, int n, double spread);

} // namespace gloview::anim