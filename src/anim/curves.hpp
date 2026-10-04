#pragma once

#include <string_view>

struct lua_State;

namespace gloview::curves {

using NativeFn = double (*)(double);

void registerBuiltins();

// hl.plugin.gloview.curve(name, fn); re-registering a name replaces it.
int luaRegister(lua_State *L);

double eval(std::string_view id, double t);

} // namespace gloview::curves
