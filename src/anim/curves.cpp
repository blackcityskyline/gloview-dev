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

// CSS-style cubic bezier: P0=(0,0), P3=(1,1), control points from the config.
// x is monotonic in t, so t is solved numerically for the requested progress.
struct Bezier {
  double x1, y1, x2, y2;
  double at(double t) const {
    const double u = 1.0 - t;
    return u * u * u * 0.0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 +
           t * t * t;
  }
  double solve(double x) const {
    double t = x; // good initial guess: control xs are usually near diagonal
    for (int i = 0; i < 8; ++i) { // Newton on cx(t) - x
      const double u  = 1.0 - t;
      const double cx = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t - x;
      const double dx = 3 * u * u * x1 + 6 * u * t * (x2 - x1) + 3 * t * t * (1 - x2);
      if (std::abs(dx) < 1e-6)
        break;
      t -= cx / dx;
      if (t <= 0.0 || t >= 1.0)
        break;
    }
    t = std::clamp(t, 0.0, 1.0);
    // bisection refinement — cheap and always converges
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 24; ++i) {
      const double u  = 1.0 - t;
      const double cx = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t;
      if (std::abs(cx - x) < 1e-5)
        break;
      (cx < x ? lo : hi) = t;
      t                  = (lo + hi) / 2.0;
    }
    return at(t);
  }
};

std::unordered_map<std::string, NativeFn> g_native;
std::unordered_map<std::string, Bezier> g_beziers;

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
  g_L = L;
  if (lua_istable(L, 2)) { // { type = "bezier", points = {{x,y},{x,y}} }
                           // or { x1, y1, x2, y2 }
    Bezier b{0.25, 0.1, 0.25, 1.0};
    lua_getfield(L, 2, "points");
    if (lua_istable(L, -1)) {
      for (int i = 0; i <= 1; ++i) {
        lua_rawgeti(L, -1, i + 1);
        if (lua_istable(L, -1)) {
          lua_rawgeti(L, -1, 1);
          double *px = i == 0 ? &b.x1 : &b.x2;
          *px        = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : *px;
          lua_pop(L, 1);
          lua_rawgeti(L, -1, 2);
          double *py = i == 0 ? &b.y1 : &b.y2;
          *py        = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : *py;
          lua_pop(L, 1);
        } else if (lua_isnumber(L, -1)) { // flat list: {x1,y1,x2,y2}
          const double v = lua_tonumber(L, -1);
          (&b.x1)[i * 2]     = v;
          lua_rawgeti(L, -2, i + 2);
          (&b.y1)[i * 2] = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
          lua_pop(L, 2);
        }
        lua_pop(L, 1);
      }
    } else { // flat numbers in the table itself
      for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, 2, i + 1);
        if (lua_isnumber(L, -1))
          (&b.x1)[i] = lua_tonumber(L, -1);
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
    g_beziers.insert_or_assign(name, b);
    g_lua.erase(name); // a redefinition replaces the other kind
    return 0;
  }
  luaL_checktype(L, 2, LUA_TFUNCTION);
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
  if (const auto it = g_beziers.find(std::string(id)); it != g_beziers.end())
    return it->second.solve(t);
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
