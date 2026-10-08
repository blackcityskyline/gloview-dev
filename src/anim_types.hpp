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

} // namespace gloview::anim