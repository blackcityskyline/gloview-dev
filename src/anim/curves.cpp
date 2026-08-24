#include "curves.hpp"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <hyprland/src/debug/log/Logger.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace gloview::curves {

namespace {

double linear(double t) { return t; }
double easeOut(double t) {
  const double inv = 1.0 - t;
  return 1.0 - inv * inv * inv; // easeOutCubic — the historical default
}
double easeInOut(double t) {
  return t < 0.5 ? 4.0 * t * t * t
                 : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}
double back(double t) {
  const double c1 = 1.70158, c3 = c1 + 1.0;
  return 1.0 + c3 * std::pow(t - 1.0, 3.0) + c1 * std::pow(t - 1.0, 2.0);
}

std::unordered_map<std::string, NativeFn> g_native;

// Lua-registered curves. g_L is the single persistent Hyprland state,
// captured on first registration; refs index into its registry table.
lua_State *g_L = nullptr;
std::unordered_map<std::string, int> g_lua;

// Fallbacks are logged ONCE per name: a broken curve must not spam the log
// at frame rate.
std::unordered_set<std::string> g_warned;
void warnOnce(const std::string &id, const std::string &what) {
  if (g_warned.insert(id).second)
    Log::logger->log(Log::WARN, "[gloview] curve '{}' {}: falling back to easeout",
                     id, what);
}

} // namespace

void registerBuiltins() {
  g_native.emplace("linear", linear);
  g_native.emplace("easeout", easeOut);
  g_native.emplace("easeinout", easeInOut);
  g_native.emplace("back", back);
}

int luaRegister(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  g_L = L;
  lua_pushvalue(L, 2);
  const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (auto it = g_lua.find(name); it != g_lua.end()) {
    luaL_unref(L, LUA_REGISTRYINDEX, it->second);
    it->second = ref;
  } else {
    g_lua.emplace(name, ref);
  }
  return 0;
}

double eval(std::string_view id, double t) {
  if (const auto it = g_native.find(std::string(id)); it != g_native.end())
    return it->second(t);
  if (g_L) {
    if (const auto it = g_lua.find(std::string(id)); it != g_lua.end()) {
      lua_rawgeti(g_L, LUA_REGISTRYINDEX, it->second);
      lua_pushnumber(g_L, t);
      if (lua_pcall(g_L, 1, 1, 0) == LUA_OK && lua_isnumber(g_L, -1))
        return lua_tonumber(g_L, -1);
      const std::string err = lua_isstring(g_L, -1)
                                  ? lua_tostring(g_L, -1)
                                  : "returned a non-number";
      lua_pop(g_L, 1);
      warnOnce(std::string(id), err);
      return easeOut(t);
    }
  }
  warnOnce(std::string(id), "is not registered");
  return easeOut(t);
}

} // namespace gloview::curves
