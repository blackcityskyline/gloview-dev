#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <hyprland/src/plugins/PluginAPI.hpp>

#include "config/config.hpp"
#include "anim_types.hpp"

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

// Cfg, Rule, Inputs and resolve() live in anim_types.hpp: they are plain values, and
// keeping them free of hyprland/ includes is what lets a test reach the resolver.
[[nodiscard]] Cfg cfg(Id id);

// Name-keyed, for the Lua layer and for config-driven lookups only. An unknown name is
// simply not an animation: Cfg{on=false}, never a default.
[[nodiscard]] Cfg cfg(const char *name);
[[nodiscard]] std::string style(const char *name);

[[nodiscard]] double param(Param p);

// Not animation styles: behaviour selectors that happen to live in the same config section.
[[nodiscard]] std::string jumpStyle();

void setRule(const char *name, const Rule &r);

int  luaSetRule(lua_State *L);
void registerAll(HANDLE handle);

} // namespace gloview::anim
