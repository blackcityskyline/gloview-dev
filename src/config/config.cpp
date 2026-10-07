#include "config.hpp"

#include "../anim.hpp"
#include "../debug/log.hpp"

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

// Hex literal: optional whitespace, `0x`/`0X`/`#`, then 6 (RRGGBB → opaque) or 8
// (AARRGGBB) digits. Anything unparseable → `fallback`.
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
// THE SCHEMA: handle, FULL config key, default. Grouped by domain — the table is
// the documentation of record for what exists and what it defaults to.
//
// The key MUST be a string LITERAL: CIntValue/CStringValue keep it as a const
// char*, so a c_str() of a temporary dangles and commence() aborts on it at
// registration — which is why the leaf keys below go into a deque.

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
    {&grid.gap, "plugin:gloview:gap", 34},
    {&grid.padding, "plugin:gloview:padding", 80},
    {&grid.padding_top, "plugin:gloview:padding_top", 40},
    {&grid.padding_bottom, "plugin:gloview:padding_bottom", 70},
    // carousel: the run, laid over `layout` in the all-workspaces view only
    {&carousel.enabled, "plugin:gloview:carousel", 0},
    {&carousel.on_open, "plugin:gloview:carousel_on_open", 1},
    {&carousel.cards, "plugin:gloview:carousel_cards", 5},
    {&carousel.card_width, "plugin:gloview:carousel_card_width", 0},
    {&strip.height, "plugin:gloview:strip_height", 150},
    {&strip.offset, "plugin:gloview:strip_offset", 0},
    {&strip.margin, "plugin:gloview:strip_margin", 22},
    {&strip.gap, "plugin:gloview:strip_gap", 18},
    {&strip.card_round, "plugin:gloview:strip_card_round", 10},
    {&strip.all_card, "plugin:gloview:strip_all_card", 0},
    {&strip.wallpaper, "plugin:gloview:strip_wallpaper", 1},
    {&strip.band_opacity, "plugin:gloview:strip_band_opacity", 70},
    {&look.preview_round, "plugin:gloview:preview_round", 12},
    {&look.show_border, "plugin:gloview:show_border", 0},
    {&look.show_focus_border, "plugin:gloview:show_focus_border", 1},
    {&look.border_size, "plugin:gloview:border_size", 2},
    {&look.hover_border_size, "plugin:gloview:hover_border_size", 3},
    {&look.select_border_size, "plugin:gloview:select_border_size", 3},
    {&blur.passes, "plugin:gloview:blur_passes", 3},
    {&blur.size, "plugin:gloview:blur_size", 8},
    {&blur.resolution, "plugin:gloview:blur_resolution", 4},
    {&keys.alt_tab_commit_on_release, "plugin:gloview:alt_tab_commit_on_release", 1},
    {&behavior.focus_follows_mouse, "plugin:gloview:focus_follows_mouse", 1},
    {&behavior.scroll_switches_workspace, "plugin:gloview:scroll_switches_workspace", 1},
    {&behavior.passthrough_keys, "plugin:gloview:passthrough_keys", 1},
    {&behavior.exit_on_click, "plugin:gloview:exit_on_click", 1},
    {&behavior.kill_zone, "plugin:gloview:kill_zone", 1},
    {&behavior.click_window_ms, "plugin:gloview:click_window_ms", 400},
    {&behavior.exit_on_switch, "plugin:gloview:exit_on_switch", 0},
    {&behavior.switch_on_drop, "plugin:gloview:switch_on_drop", 0},
    {&behavior.drag_to_swap, "plugin:gloview:drag_to_swap", 1},
    {&behavior.switch_on_new_workspace, "plugin:gloview:switch_on_new_workspace", 1},
    {&behavior.show_all_workspaces, "plugin:gloview:show_all_workspaces", 0},
    {&behavior.show_special, "plugin:gloview:show_special", 0},
    {&behavior.alttab_advances, "plugin:gloview:alttab_advances", 0},
    {&behavior.hold_lift_ms, "plugin:gloview:hold_lift_ms", 400},
    {&layer.hide_top, "plugin:gloview:hide_top_layers", 0},
    {&layer.hide_overlay, "plugin:gloview:hide_overlay_layers", 0},
    {&debug.logs, "plugin:gloview:debug_logs", 0},
};

constexpr FloatSpec kFloats[] = {
    {&look.shadow_radius, "plugin:gloview:shadow_radius", 6.0},
    {&look.shadow_radius_lift, "plugin:gloview:shadow_radius_lift", 14.0},
    {&look.shadow_alpha, "plugin:gloview:shadow_alpha", 0.18},
    {&look.title_pad_x, "plugin:gloview:title_pad_x", 14.0},
    {&look.title_pad_y, "plugin:gloview:title_pad_y", 6.0},
    {&look.title_gap, "plugin:gloview:title_gap", 10.0},
    {&look.title_margin, "plugin:gloview:title_margin", 6.0},
    {&look.close_glyph_fill, "plugin:gloview:close_glyph_fill", 0.62},
    {&strip.label_band, "plugin:gloview:strip_label_band", 26.0},
    {&strip.label_extra, "plugin:gloview:strip_label_extra", 24.0},
    {&strip.slide_frac, "plugin:gloview:strip_slide_frac", 0.55},
    {&strip.offset_max, "plugin:gloview:strip_offset_max", 0.42},

    {&behavior.kill_zone_size, "plugin:gloview:kill_zone_size", 40.0},
    {&behavior.kill_zone_falloff, "plugin:gloview:kill_zone_falloff", 150.0},
    {&grid.max_scale, "plugin:gloview:max_scale", 1.0F},
    {&look.preview_round_power, "plugin:gloview:preview_round_power", 2.0F},
    {&blur.strength, "plugin:gloview:blur", 1.0F},
    {&look.close_button_size, "plugin:gloview:close_button_size", 1.0F},
    {&look.drag_size, "plugin:gloview:drag_size", 0.55F},
    {&carousel.scale, "plugin:gloview:carousel_scale", 1.0F},
    {&carousel.scroll_speed, "plugin:gloview:carousel_scroll_speed", 1.0F},
};

constexpr StrSpec kStrs[] = {
    {&grid.layout, "plugin:gloview:layout", "rows"},
    {&strip.anchor, "plugin:gloview:anchor", ""},
    {&strip.bar_position, "plugin:gloview:bar_position", "top"}, // deprecated alias
    {&strip.empty_mode, "plugin:gloview:strip_empty_mode", "show"},
    {&look.close_button_position, "plugin:gloview:close_button_position", "top-right"},
    {&look.close_button_visibility, "plugin:gloview:close_button_visibility", "shift"},
    {&look.close_trigger, "plugin:gloview:close_trigger", "button"},
    {&look.close_button_icon, "plugin:gloview:close_button_icon", "\xe2\x9c\x95"},
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
    {&keys.carousel, "plugin:gloview:key_carousel", "c"},
    {&keys.workspace, "plugin:gloview:key_workspace", "1,2,3,4,5,6,7,8,9,0"},
    {&keys.alt_tab_modifier, "plugin:gloview:alt_tab_modifier", "alt"},
    {&behavior.preview_mode, "plugin:gloview:preview_mode", "live"},
    {&behavior.tile_layout,  "plugin:gloview:tile_layout",  "none"},
    {&behavior.cursor_mode, "plugin:gloview:cursor_mode", "auto"},
    {&behavior.workspace_key_mode, "plugin:gloview:key_workspace_mode", "switch"},
    {&behavior.new_workspace_mode, "plugin:gloview:new_workspace_mode", "fill"},
    {&layer.above_namespaces, "plugin:gloview:above_namespaces", ""},
};

constexpr ColorSpec kColors[] = {
    {&blur.backdrop, "plugin:gloview:backdrop_color", "0x73070a10"},
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
    {&colors.carousel_count_pill, "plugin:gloview:carousel_count_pill_color", "0xcc11151c"},
};

} // namespace

// ---- animation leaves ---------------------------------------------------------
// One row per leaf: handles, base duration (what speed 1.0 means), default curve.
// The resolver reads ONLY this table and the imperative rules.
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
  anim::registerAll(handle);
}

} // namespace gloview
