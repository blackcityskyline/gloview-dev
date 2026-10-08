// The animation precedence chain, in its own translation unit.
//
// This exists as a separate .cpp for one reason: anim.cpp cannot be linked into a test.
// It includes the Lua headers and reads Config::Values, both of which resolve inside the
// Hyprland binary -- there is no libhyprland to link against. resolve() touches none of
// that, so it lives here where a test can reach it. cfg() above it is the thin shell that
// reads the handles and calls in.
//
// Nothing in here may include anything from hyprland/. That rule is what keeps the test
// possible, and a violation shows up as a link error in tests/anim_resolve_check.cpp.

#include "anim_types.hpp"

#include <algorithm>

namespace gloview::anim {

Cfg resolve(const Inputs &in, int baseMs, const char *curveDefault) {
  Cfg c{};
  double speed = in.speed;
  int absMs = in.hasMs ? in.ms : -1;

  c.on = in.globalEnabled && in.enabled;
  if (in.rule) {
    if (in.rule->enabled) c.on = *in.rule->enabled;
    if (in.rule->speed)   speed = *in.rule->speed;
    if (in.rule->ms)      absMs = *in.rule->ms;
  }
  if (!c.on) {
    // 1ms, not 0: the clock still has to tick, it just must not move anything.
    c.ms = 1.0;
    return c;
  }
  // _ms IS the base duration -- it defaults to the table's base, so existing configs are
  // unchanged -- and _speed still scales it. A Lua rule's ms overrides both outright.
  const double base = absMs >= 0 ? static_cast<double>(absMs) : static_cast<double>(baseMs);
  c.ms = std::max(16.0, base / std::clamp(speed, 0.01, 20.0));
  if (in.rule && in.rule->curve)
    c.curve = *in.rule->curve;
  else if (!in.curve.empty())
    c.curve = in.curve;
  else if (curveDefault)
    c.curve = curveDefault;
  return c;
}

double resolveParam(bool hasValue, double value, double fallback) {
  return hasValue ? value : fallback;
}

} // namespace gloview::anim