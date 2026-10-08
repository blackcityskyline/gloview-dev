#pragma once

#include <string_view>

struct lua_State;

namespace gloview::curves {

using NativeFn = double (*)(double);

void registerBuiltins();

int luaRegister(lua_State *L);

double eval(std::string_view id, double t);

} // namespace gloview::curves
