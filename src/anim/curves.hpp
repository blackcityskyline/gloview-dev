#pragma once

// The curve registry — the seam between the animation core and the curves
// themselves (REFACTORING.md A1). Animation leaves carry a curve NAME (the
// <leaf>_curve config string); this registry resolves names to evaluators.
// The core never shapes time itself: clocks produce linear progress, and
// every consumer asks the registry for the shaped value.
//
// Two evaluator kinds:
//   * native — plain functions registered at plugin init (zero overhead);
//   * Lua — functions stored via the hl.plugin.gloview.curve(name, fn) API,
//     evaluated with a pcall per use (a few calls per frame; negligible).
//
// Contract: t arrives in 0..1; the return value is NOT clamped — overshoot
// (back-style curves) is legal and consumed by call sites that want it.
// Unknown names and Lua errors fall back to "easeout" (the historical
// default), logging once per name.

#include <string_view>

struct lua_State;

namespace gloview::curves {

using NativeFn = double (*)(double);

void registerBuiltins();

// Lua entry: hl.plugin.gloview.curve(name, fn). Stores a registry reference
// to fn under name (re-registering a name replaces it). Assumes Hyprland's
// single persistent lua_State (the same one config scripts run on).
int luaRegister(lua_State *L);

double eval(std::string_view id, double t);

} // namespace gloview::curves
