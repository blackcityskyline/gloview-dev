#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <hyprland/src/plugins/PluginAPI.hpp>

#include "config/config.hpp"

// THE animation registry.
//
// Every animation the plugin has is declared EXACTLY ONCE, in the kRows table in anim.cpp.
// The enum id, the config keys and the defaults are all one row, so they cannot drift apart.
// The previous design had them in three places (a struct in config.hpp, a table row in
// config.cpp, a string literal at each call site) and drifted for real: leaf("new_card")
// named an animation that was never registered and silently answered 250ms/easeout.
//
// Nothing here substitutes a default for an animation that is not there. An unknown or
// unbound id resolves to Cfg{on=false}: the motion does not happen, instead of happening at
// some arbitrary duration nobody asked for.
namespace gloview::anim {

enum class Id : std::uint8_t {
  Open, Close, Glide, Appear, Card, Pulse, StripStep,
  WsEnter, WsExit, ExpoIn, ExpoOut, JumpIn, JumpOut,
  SwapMain, SwapPartner, Drop, SwapFly, Lift, CarouselStep, NewCard,
  Count
};

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

[[nodiscard]] const char *name(Id id);

// Resolved configuration of one registered animation.
[[nodiscard]] Cfg cfg(Id id);

// Name-keyed, for the Lua layer and for config-driven lookups only. An unknown name is
// simply not an animation: Cfg{on=false}, never a default.
[[nodiscard]] Cfg cfg(const char *name);
[[nodiscard]] std::string style(const char *name);

// SHAPE parameters: not how long an animation runs but how it is shaped. They belong to an
// animation and live beside it here, rather than as literals in the render code that
// consumes them. Resolved live, exactly like the rest of the registry.
enum class Param : std::uint8_t {
  Stagger,        // entry ladder: spread of the tiles over the window
  StaggerPerTile, // ... per extra tile, capped by Stagger
  Collapse,       // carousel: fraction of the exit spent shrinking the unfocused cards
  Count
};
[[nodiscard]] double param(Param p);

// Not animation styles: behaviour selectors that happen to live in the same config section.
[[nodiscard]] const std::string &gridSwapStyle();
[[nodiscard]] const std::string &stripSwapStyle();
[[nodiscard]] const std::string &jumpStyle();

void setRule(const char *name, const Rule &r);

int  luaSetRule(lua_State *L);
void registerAll(HANDLE handle);

} // namespace gloview::anim
