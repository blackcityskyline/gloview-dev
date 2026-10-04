#pragma once

// Curve registry: resolves a curve NAME (the <leaf>_curve config value) to an
// evaluator. Clocks produce linear progress; every consumer shapes it here.
// Return value is unclamped (back-style overshoot is legal). Unknown names and
// Lua errors fall back to "easeout", logging once per name.

#include <string_view>

struct lua_State;

namespace gloview::curves {

using NativeFn = double (*)(double);

void registerBuiltins();

// hl.plugin.gloview.curve(name, fn); re-registering a name replaces it.
int luaRegister(lua_State *L);

double eval(std::string_view id, double t);

} // namespace gloview::curves
