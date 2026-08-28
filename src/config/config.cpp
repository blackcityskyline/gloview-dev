#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <optional>
#include <unordered_map>

#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace gloview::cfg {

// ---- Color ------------------------------------------------------------------

namespace {

// Hex-literal grammar (verified, was parseHexColor): optional whitespace,
// `0x`/`0X`/`#` prefix, then 6 (RRGGBB → opaque) or 8 (AARRGGBB) hex digits.
// Anything unparseable → `fallback`.
uint64_t parseHex(std::string s, uint64_t fallback) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.pop_back();
  if (s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    s.erase(0, 2);
  else if (!s.empty() && s[0] == '#')
    s.erase(0, 1);
  if (s.size() != 6 && s.size() != 8)
    return fallback;
  for (const char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return fallback;
  try {
    return std::stoull(s, nullptr, 16);
  } catch (...) {
    return fallback;
  }
}

CHyprColor parseColor(std::string s, const char *fallback) {
  const uint64_t v = parseHex(std::move(s), parseHex(fallback, 0xffffffffULL));
  const auto a = static_cast<double>((v >> 24) & 0xFF) / 255.0;
  const auto r = static_cast<double>((v >> 16) & 0xFF) / 255.0;
  const auto g = static_cast<double>((v >> 8) & 0xFF) / 255.0;
  const auto b = static_cast<double>(v & 0xFF) / 255.0;
  return CHyprColor(r, g, b, a);
}

// Per-option parse cache, keyed by the handle's address (Color handles are
// inline globals — one stable instance per option). A string compare per
// frame beats a re-parse plus a temporary std::string by a wide margin.
// Main thread only (config writes and paint both run there).
std::unordered_map<const Color *, std::pair<std::string, CHyprColor>>
    g_colorCache;

} // namespace

CHyprColor Color::get(float alphaMul) const {
  const std::string raw = v ? std::string(v->value()) : std::string(def);
  auto &c = g_colorCache[this];
  if (c.first != raw) {
    c.first  = raw;
    c.second = parseColor(raw, def);
  }
  CHyprColor out = c.second;
  out.a *= std::clamp(alphaMul, 0.0F, 1.0F);
  return out;
}

// ---- registration -------------------------------------------------------------
// THE SCHEMA. One row per option: handle, FULL config key, default.
// The key MUST be a string LITERAL: CIntValue/CStringValue keep the
// name as a const char* — a c_str() of a temporary std::string dangles
// and commence() aborts on it at registration (session-fatal). Grouped and commented by domain — this table
// is the documentation of record for what exists and what it defaults to.

namespace {



struct IntSpec {
  Int *h;
  const char *key;
  int def;
};
struct FloatSpec {
  Float *h;
  const char *key;
  float def;
};
struct StrSpec {
  Str *h;
  const char *key;
  const char *def;
};
struct ColorSpec {
  Color *h;
  const char *key;
  const char *def;
};

constexpr IntSpec kInts[] = {
    // grid
    {&grid.gap, "plugin:gloview:gap", 34},
    {&grid.padding, "plugin:gloview:padding", 80},
    {&grid.padding_top, "plugin:gloview:padding_top", 40},
    {&grid.padding_bottom, "plugin:gloview:padding_bottom", 70},
    // strip
    {&strip.height, "plugin:gloview:strip_height", 150},
    {&strip.offset, "plugin:gloview:strip_offset", 0},
    {&strip.margin, "plugin:gloview:strip_margin", 22},
    {&strip.gap, "plugin:gloview:strip_gap", 18},
    {&strip.card_round, "plugin:gloview:strip_card_round", 10},
    {&strip.all_card, "plugin:gloview:strip_all_card", 0},
    {&strip.wallpaper, "plugin:gloview:strip_wallpaper", 1},
    // look
    {&look.preview_round, "plugin:gloview:preview_round", 12},
    {&look.show_border, "plugin:gloview:show_border", 0},
    {&look.show_focus_border, "plugin:gloview:show_focus_border", 1},
    {&look.border_size, "plugin:gloview:border_size", 2},
    {&look.hover_border_size, "plugin:gloview:hover_border_size", 3},
    {&look.select_border_size, "plugin:gloview:select_border_size", 3},
    // blur
    {&blur.passes, "plugin:gloview:blur_passes", 3},
    {&blur.size, "plugin:gloview:blur_size", 8},
    {&blur.resolution, "plugin:gloview:blur_resolution", 4},
    {&blur.fullscreen_background, "plugin:gloview:fullscreen_background", 0},
    {&blur.frost_underlay, "plugin:gloview:frost_underlay", 0}, // DEPRECATED no-op
    // anim
    {&anim::enabled, "plugin:gloview:animations_enabled", 1},
    // keys
    {&keys.alt_tab_commit_on_release, "plugin:gloview:alt_tab_commit_on_release", 1},
    // behavior
    {&behavior.focus_follows_mouse, "plugin:gloview:focus_follows_mouse", 1},
    {&behavior.scroll_switches_workspace, "plugin:gloview:scroll_switches_workspace", 1},
    {&behavior.passthrough_keys, "plugin:gloview:passthrough_keys", 1},
    {&behavior.exit_on_click, "plugin:gloview:exit_on_click", 1},
    {&behavior.exit_on_switch, "plugin:gloview:exit_on_switch", 0},
    {&behavior.switch_on_drop, "plugin:gloview:switch_on_drop", 0},
    {&behavior.drag_to_swap, "plugin:gloview:drag_to_swap", 1},
    {&behavior.switch_on_new_workspace, "plugin:gloview:switch_on_new_workspace", 1},
    {&behavior.show_all_workspaces, "plugin:gloview:show_all_workspaces", 0},
    {&behavior.show_special, "plugin:gloview:show_special", 0},
    {&behavior.hold_lift_ms, "plugin:gloview:hold_lift_ms", 400},
    // layer
    {&layer.hide_top, "plugin:gloview:hide_top_layers", 0},
    {&layer.hide_overlay, "plugin:gloview:hide_overlay_layers", 0},
    // debug
    {&debug.logs, "plugin:gloview:debug_logs", 0},
};

constexpr FloatSpec kFloats[] = {
    {&grid.max_scale, "plugin:gloview:max_scale", 1.0F},
    {&look.preview_round_power, "plugin:gloview:preview_round_power", 2.0F},
    {&blur.strength, "plugin:gloview:blur", 1.0F},
    {&look.close_button_size, "plugin:gloview:close_button_size", 1.0F},
    {&look.drag_size, "plugin:gloview:drag_size", 0.55F},
};

constexpr StrSpec kStrs[] = {
    // grid / strip / look
    {&grid.layout, "plugin:gloview:layout", "rows"},
    {&strip.anchor, "plugin:gloview:anchor", ""},
    {&strip.bar_position, "plugin:gloview:bar_position", "top"}, // deprecated alias
    {&strip.empty_mode, "plugin:gloview:strip_empty_mode", "show"},
    {&look.close_button_position, "plugin:gloview:close_button_position", "top-right"},
    {&look.close_button_visibility, "plugin:gloview:close_button_visibility", "shift"},
    {&look.close_trigger, "plugin:gloview:close_trigger", "button"},
    {&look.close_button_icon, "plugin:gloview:close_button_icon", "\xe2\x9c\x95"},
    // keys
    {&keys.close, "plugin:gloview:key_close", "escape"},
    {&keys.next_workspace, "plugin:gloview:key_next_workspace", "tab"},
    {&keys.prev_workspace, "plugin:gloview:key_prev_workspace", "shift+tab"},
    {&keys.activate, "plugin:gloview:key_activate", "enter"},
    {&keys.close_window, "plugin:gloview:key_close_window", "d"},
    {&keys.left, "plugin:gloview:key_left", "left"},
    {&keys.right, "plugin:gloview:key_right", "right"},
    {&keys.up, "plugin:gloview:key_up", "up"},
    {&keys.down, "plugin:gloview:key_down", "down"},
    {&keys.desktop, "plugin:gloview:key_desktop", "shift"},
    {&keys.all_workspaces, "plugin:gloview:key_all_workspaces", "a"},
    {&keys.workspace, "plugin:gloview:key_workspace", "1,2,3,4,5,6,7,8,9,0"},
    {&keys.alt_tab_modifier, "plugin:gloview:alt_tab_modifier", "alt"},
    // behavior
    {&behavior.preview_mode, "plugin:gloview:preview_mode", "live"},
    {&behavior.tile_layout,  "plugin:gloview:tile_layout",  "none"},
    {&anim::grid_swap_style, "plugin:gloview:grid_swap_style", "horizontal"},
    {&anim::strip_swap_style, "plugin:gloview:strip_swap_style", "horizontal"},
    {&anim::ws_enter_style, "plugin:gloview:ws_enter_style", "slide"},
    {&anim::ws_exit_style, "plugin:gloview:ws_exit_style", "slide"},
    {&behavior.cursor_mode, "plugin:gloview:cursor_mode", "auto"},
    {&behavior.workspace_key_mode, "plugin:gloview:key_workspace_mode", "switch"},
    {&behavior.new_workspace_mode, "plugin:gloview:new_workspace_mode", "fill"},
    // layer
    {&layer.above_namespaces, "plugin:gloview:above_namespaces", ""},
};

constexpr ColorSpec kColors[] = {
    {&blur.backdrop, "plugin:gloview:backdrop_color", "0x73070a10"},
    {&colors.backing, "plugin:gloview:backing_color", "0xff14181f"},
    {&colors.border, "plugin:gloview:border_color", "0x50ffffff"},
    {&colors.close_button, "plugin:gloview:close_button_color", "0xe6e23b3b"},
    {&colors.close_glyph, "plugin:gloview:close_glyph_color", "0xffffffff"},
    {&colors.drop_hint, "plugin:gloview:drop_hint_color", "0x38ffffff"},
    {&colors.hover_border, "plugin:gloview:hover_border", "0xf0ffffff"},
    {&colors.label, "plugin:gloview:label_color", "0xf2ffffff"},
    {&colors.select_border, "plugin:gloview:select_border", "0xf066ccff"},
    {&colors.shadow, "plugin:gloview:shadow_color", "0x70000000"},
    {&colors.strip_active, "plugin:gloview:strip_active_color", "0x4d1c2c44"},
    {&colors.strip_active_border, "plugin:gloview:strip_active_border", "0xf0ffffff"},
    {&colors.strip_all, "plugin:gloview:strip_all_color", "0xd0eef4ff"},
    {&colors.strip_band, "plugin:gloview:strip_band_color", "0x24ffffff"},
    {&colors.strip_card, "plugin:gloview:strip_card_color", "0x3a0e131c"},
    {&colors.strip_hover, "plugin:gloview:strip_hover_border", "0x80ffffff"},
    {&colors.strip_plus, "plugin:gloview:strip_plus_color", "0xd0eef4ff"},
    {&colors.title_pill, "plugin:gloview:title_pill_color", "0xcc11151c"},
};

} // namespace

// ---- animation leaves ---------------------------------------------------------
// One row per leaf: handles, built-in base duration (what speed 1.0 means) and
// the default curve. The resolver (anim/clocks.cpp) reads ONLY this table and
// the imperative rules — adding a leaf is one row here plus consumers.
constexpr LeafSpec kLeaves[] = {
    {"open", &anim::open, 300, "easeout"},         {"close", &anim::close, 300, "easeout"},
    {"glide", &anim::glide, 300, "easeout"},       {"appear", &anim::appear, 250, "easeout"},
    {"card", &anim::card, 250, "back"},            {"pulse", &anim::pulse, 180, "back"},
    {"strip_step", &anim::strip_step, 200, "easeinout"},
    {"ws_enter", &anim::ws_enter, 250, "easeout"}, {"ws_exit", &anim::ws_exit, 250, "easeout"},
    {"expo_in", &anim::expo_in, 250, "easeout"},   {"expo_out", &anim::expo_out, 250, "easeout"},
    {"jump_in",  &anim::jump_in,  180, "easeout"}, {"jump_out", &anim::jump_out, 220, "easeout"},
    {"swap_main", &anim::swap_main, 320, "easeinout"},
    {"swap_partner", &anim::swap_partner, 320, "easeinout"},
    {"drag", &anim::drag, 400, "easeout"},
};

void registerLeaf(HANDLE handle, const LeafSpec &l) {
  // deque: element addresses stay valid across appends — the keys MUST
  // outlive registration (CIntValue/CStringValue keep them as const char*).
  static std::deque<std::string> keys;
  const auto key = [&](const char *suffix) {
    keys.emplace_back(std::string("plugin:gloview:") + l.name + "_" + suffix);
    return keys.back().c_str();
  };
  auto reg = [](HANDLE h, const char *k, auto v) {
    HyprlandAPI::addConfigValueV2(h, v);
  };
  // clang-format off
  l.leaf->enabled.v = makeShared<Config::Values::CIntValue>(key("enabled"), "", static_cast<Config::INTEGER>(1));
  l.leaf->enabled.def = 1;
  reg(handle, key("enabled"), l.leaf->enabled.v);
  l.leaf->speed.v = makeShared<Config::Values::CFloatValue>(key("speed"), "", 1.0F);
  l.leaf->speed.def = 1.0F;
  reg(handle, key("speed"), l.leaf->speed.v);
  l.leaf->curve.v = makeShared<Config::Values::CStringValue>(key("curve"), "", l.curveDef);
  l.leaf->curve.def = l.curveDef;
  reg(handle, key("curve"), l.leaf->curve.v);
  // clang-format on
}

const LeafSpec *animLeaf(const char *name) {
  for (const auto &l : kLeaves)
    if (std::strcmp(l.name, name) == 0)
      return &l;
  return nullptr;
}

void registerAll(HANDLE handle) {
  for (auto &s : kInts) {
    s.h->v = makeShared<Config::Values::CIntValue>(s.key, "",
                                                     static_cast<Config::INTEGER>(s.def));
    s.h->def = s.def;
    HyprlandAPI::addConfigValueV2(handle, s.h->v);
  }
  for (auto &s : kFloats) {
    s.h->v = makeShared<Config::Values::CFloatValue>(s.key,
                                                     "", s.def);
    s.h->def = s.def;
    HyprlandAPI::addConfigValueV2(handle, s.h->v);
  }
  for (auto &s : kStrs) {
    s.h->v = makeShared<Config::Values::CStringValue>(s.key, "",
                                                        s.def);
    s.h->def = s.def;
    HyprlandAPI::addConfigValueV2(handle, s.h->v);
  }
  for (auto &s : kColors) {
    s.h->v = makeShared<Config::Values::CStringValue>(s.key, "",
                                                        s.def);
    s.h->def = s.def;
    HyprlandAPI::addConfigValueV2(handle, s.h->v);
  }
  // Leaf keys are BUILT here, not written as literals — but the names are
  // kept alive in a deque (stable element addresses) because CIntValue stores
  // the key as const char*; a dangling c_str() of a temporary is the
  // session-fatal trap documented at the schema tables above.
  for (const auto &l : kLeaves)
    registerLeaf(handle, l);
}


namespace {
std::unordered_map<std::string, AnimRule> g_rules;
} // namespace

void setAnimRule(const std::string &leaf, const AnimRule &rule) { g_rules[leaf] = rule; }
const AnimRule *animRule(const std::string &leaf) {
  const auto it = g_rules.find(leaf);
  return it == g_rules.end() ? nullptr : &it->second;
}

std::string animStyle(const std::string &thing) {
  static const std::unordered_map<std::string, const Str *> k = {
      {"ws_enter", &anim::ws_enter_style},  {"ws_exit", &anim::ws_exit_style},
      {"grid_swap", &anim::grid_swap_style},{"strip_swap", &anim::strip_swap_style},
  };
  if (const auto *r = animRule(thing); r && r->style)
    return *r->style;
  const auto it = k.find(thing);
  return it == k.end() ? std::string() : it->second->get();
}

// gloview.animation({ leaf = "...", enabled = bool, speed = n, ms = n,
//                     curve = "...", style = "..." }) — an imperative rule
// layered over the schema keys; whatever it sets wins.
int luaSetAnimRule(::lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);
  lua_getfield(L, 1, "leaf");
  const char *leaf = luaL_checkstring(L, -1);
  lua_pop(L, 1);
  AnimRule r;
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
  setAnimRule(leaf, r);
  return 0;
}

} // namespace gloview
