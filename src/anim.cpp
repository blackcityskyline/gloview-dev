#include "anim.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <unordered_map>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "debug/log.hpp"

namespace gloview::anim {

// One row per animation: id, config key stem, defaults, and whether it has a style. This
// array IS the registry -- indexed by Id, so an id cannot exist without a row.
struct Row {
  const char *name;
  cfg::Int    enabled;
  cfg::Float  speed;
  cfg::Str    curve;
  cfg::Str    style;
  cfg::Int    ms;      // absolute duration; _speed still scales it
  float       base;
  const char *curveDef;
  bool        hasStyle;
  const char *styleDefault;
};

static std::array<Row, static_cast<std::size_t>(Id::Count)> kRows = {{
{"open", {}, {}, {}, {}, {}, 500, "easeout", false, ""},
{"close", {}, {}, {}, {}, {}, 500, "easeout", false, ""},
{"glide", {}, {}, {}, {}, {}, 300, "easeout", false, ""},
{"appear", {}, {}, {}, {}, {}, 250, "easeout", false, ""},
{"card", {}, {}, {}, {}, {}, 250, "back", false, ""},
{"pulse", {}, {}, {}, {}, {}, 180, "back", false, ""},
{"strip_step", {}, {}, {}, {}, {}, 200, "easeinout", false, ""},
{"ws_enter", {}, {}, {}, {}, {}, 250, "easeout", true, "slide"},
{"ws_exit", {}, {}, {}, {}, {}, 250, "easeout", true, "slide"},
{"expo_in", {}, {}, {}, {}, {}, 250, "easeout", false, ""},
{"expo_out", {}, {}, {}, {}, {}, 250, "easeout", false, ""},
{"jump_in", {}, {}, {}, {}, {}, 180, "easeout", false, ""},
{"jump_out", {}, {}, {}, {}, {}, 220, "easeout", false, ""},
{"swap_main", {}, {}, {}, {}, {}, 320, "easeinout", false, ""},
{"swap_partner", {}, {}, {}, {}, {}, 320, "easeinout", false, ""},
  // One animation per ACTION, not one per feeling. These three shared "drag" while meaning
  // a landing, a fly-between-slots and a raise; config could not tell them apart, and a
  // single _speed therefore moved all three at once.
{"drop", {}, {}, {}, {}, {}, 400, "easeout", false, ""},
{"swap_fly", {}, {}, {}, {}, {}, 400, "easeout", false, ""},
{"lift", {}, {}, {}, {}, {}, 400, "easeout", false, ""},
{"carousel_step", {}, {}, {}, {}, {}, 300, "easeout", false, ""},
{"new_card", {}, {}, {}, {}, {}, 250, "easeout", false, ""},
}};

static cfg::Int g_globallyEnabled;
static const std::string s_grid = "horizontal", s_strip = "horizontal", s_jump = "instant";
static cfg::Str g_gridSwapStyle, g_stripSwapStyle, g_jumpStyle;

// Shape parameters, in the same order as anim::Param.
static cfg::Float g_params[static_cast<std::size_t>(Param::Count)];
static const char *const kParamKeys[] = {"appear_stagger", "appear_stagger_per_tile",
                                          "carousel_step_collapse",
                                          "pulse_ring_scale",
                                          "lift_scale", "lift_rise",
                                          "appear_pop_scale",
                                          "shadow_offset", "lift_shadow_offset"};
// The last three were literals in the render code: the scale-in depth, and the tile
// shadow offset at rest and while lifted. Shape, not timing. The shadow RADIUS was
// already configurable (shadow_radius / shadow_radius_lift); the offset was not.
static const double kParamDefaults[] = {0.08, 0.015, 0.5, 0.10, 0.07, 5.0,
                                        0.15, 16.0, 30.0};

namespace {
std::unordered_map<std::string, Rule> g_rules;

const Row *find(const char *n) {
  if (!n)
    return nullptr;
  for (const auto &r : kRows)
    if (std::strcmp(r.name, n) == 0)
      return &r;
  return nullptr;
}

const Rule *ruleFor(const char *n) {
  const auto it = g_rules.find(n);
  return it == g_rules.end() ? nullptr : &it->second;
}
} // namespace

const char *name(Id id) { return kRows[static_cast<std::size_t>(id)].name; }

Cfg cfg(Id id) { return cfg(kRows[static_cast<std::size_t>(id)].name); }

Cfg cfg(const char *n) {
  Cfg c{};
  const Row *r = find(n);
  // Not an animation: no motion, and crucially no invented duration.
  if (!r)
    return c;

  double speed = r->speed ? static_cast<double>(r->speed.get()) : 1.0;
  int absMs = -1;
  c.on = g_globallyEnabled ? (g_globallyEnabled != 0) : true;
  if (r->enabled && r->enabled.get() == 0)
    c.on = false;
  if (const auto *rule = ruleFor(r->name)) {
    if (rule->enabled)  c.on = *rule->enabled;
    if (rule->speed)    speed = *rule->speed;
    if (rule->ms)       absMs = *rule->ms;
  }
  if (!c.on) {
    c.ms = 1.0;
    return c;
  }
  // The key IS the base duration -- it defaults to the table's base, so existing configs
  // are unchanged -- and _speed still scales it. A Lua rule's ms overrides both outright.
  const double base = absMs >= 0 ? static_cast<double>(absMs)
                                  : static_cast<double>(r->ms.v ? r->ms.get() : r->base);
  c.ms = std::max(16.0, base / std::clamp(speed, 0.01, 20.0));
  c.curve = (ruleFor(r->name) && ruleFor(r->name)->curve)
                ? *ruleFor(r->name)->curve
                : (r->curve.v ? r->curve.get() : std::string(r->curveDef));
  return c;
}

std::string style(const char *n) {
  const Row *r = find(n);
  if (!r || !r->hasStyle)
    return {};
  if (const auto *rule = ruleFor(r->name); rule && rule->style)
    return *rule->style;
  return r->style.v ? r->style.get() : std::string();
}

void setRule(const char *n, const Rule &rule) {
  if (n)
    g_rules[n] = rule;
}

double param(Param p) {
  const auto i = static_cast<std::size_t>(p);
  auto &h = g_params[i];
  return h.v ? h.get() : kParamDefaults[i];
}

// By value, not by reference: Str::get() returns std::string by value, so the ternary
// produces a temporary and a const& return would dangle. gridSwapStyle/stripSwapStyle
// had the same shape and no callers at all -- style() reaches those directly -- so they
// are gone rather than fixed.
std::string jumpStyle() { return g_jumpStyle.v ? g_jumpStyle.get() : s_jump; }

void registerAll(HANDLE handle) {
  // deque: the keys must outlive registration (CIntValue keeps them as const char*).
  static std::deque<std::string> keys;
  const auto key = [&](const char *stem, const char *suffix) {
    keys.emplace_back(std::string("plugin:gloview:") + stem + suffix);
    return keys.back().c_str();
  };

  g_globallyEnabled.v = makeShared<Config::Values::CIntValue>(
      key("animations", "_enabled"), "", static_cast<Config::INTEGER>(1));
  g_globallyEnabled.def = 1;
  HyprlandAPI::addConfigValueV2(handle, g_globallyEnabled.v);

  const auto plain = [&](const char *stem, const char *def) {
    auto &h = (std::strcmp(stem, "grid_swap_style") == 0)   ? g_gridSwapStyle
             : (std::strcmp(stem, "strip_swap_style") == 0) ? g_stripSwapStyle
                                                            : g_jumpStyle;
    h.v = makeShared<Config::Values::CStringValue>(key("", stem), "", def);
    h.def = def;
    HyprlandAPI::addConfigValueV2(handle, h.v);
  };
  plain("grid_swap_style", "horizontal");
  plain("strip_swap_style", "horizontal");
  plain("jump_style", "instant");

  for (std::size_t i = 0; i < static_cast<std::size_t>(Param::Count); ++i) {
    g_params[i].v = makeShared<Config::Values::CFloatValue>(
        key("", kParamKeys[i]), "", static_cast<float>(kParamDefaults[i]));
    g_params[i].def = kParamDefaults[i];
    HyprlandAPI::addConfigValueV2(handle, g_params[i].v);
  }

  for (auto &r : kRows) {
    r.enabled.v = makeShared<Config::Values::CIntValue>(key(r.name, "_enabled"), "",
                                                       static_cast<Config::INTEGER>(1));
    r.enabled.def = 1;
    HyprlandAPI::addConfigValueV2(handle, r.enabled.v);
    r.speed.v = makeShared<Config::Values::CFloatValue>(key(r.name, "_speed"), "", 1.0F);
    r.speed.def = 1.0F;
    HyprlandAPI::addConfigValueV2(handle, r.speed.v);
    r.ms.v = makeShared<Config::Values::CIntValue>(key(r.name, "_ms"), "",
                                                  static_cast<Config::INTEGER>(r.base));
    r.ms.def = static_cast<int>(r.base);
    HyprlandAPI::addConfigValueV2(handle, r.ms.v);
    r.curve.v = makeShared<Config::Values::CStringValue>(key(r.name, "_curve"), "",
                                                         r.curveDef);
    r.curve.def = r.curveDef;
    HyprlandAPI::addConfigValueV2(handle, r.curve.v);
    if (r.hasStyle) {
      r.style.v = makeShared<Config::Values::CStringValue>(key(r.name, "_style"), "",
                                                          r.styleDefault);
      r.style.def = r.styleDefault;
      HyprlandAPI::addConfigValueV2(handle, r.style.v);
    }
  }
}

int luaSetRule(::lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);
  lua_getfield(L, 1, "leaf");
  const char *leaf = luaL_checkstring(L, -1);
  lua_pop(L, 1);
  Rule r;
  auto num = [&](const char *k) -> std::optional<double> {
    lua_getfield(L, 1, k);
    const bool ok = lua_isnumber(L, -1);
    const double v = ok ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    return ok ? std::optional(v) : std::nullopt;
  };
  auto str = [&](const char *k) -> std::optional<std::string> {
    lua_getfield(L, 1, k);
    const bool ok = lua_isstring(L, -1);
    const std::string v = ok ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return ok ? std::optional(v) : std::nullopt;
  };
  lua_getfield(L, 1, "enabled");
  if (lua_isboolean(L, -1))
    r.enabled = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  if (const auto v = num("speed"))
    r.speed = *v;
  if (const auto v = num("ms"))
    r.ms = static_cast<int>(*v);
  r.curve = str("curve");
  r.style = str("style");
  setRule(leaf, r);
  return 0;
}
} // namespace gloview::anim
