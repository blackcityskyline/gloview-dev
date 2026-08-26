#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include <hyprland/src/plugins/PluginAPI.hpp>

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
    {&anim.enabled, "plugin:gloview:animations_enabled", 1},
    {&anim.duration, "plugin:gloview:duration", 360},
    {&anim.open_enabled, "plugin:gloview:open_enabled", 1},
    {&anim.open_ms, "plugin:gloview:open_ms", -1},
    {&anim.close_enabled, "plugin:gloview:close_enabled", 1},
    {&anim.close_ms, "plugin:gloview:close_ms", -1},
    {&anim.reflow_enabled, "plugin:gloview:reflow_enabled", 1},
    {&anim.reflow_ms, "plugin:gloview:reflow_ms", -1},
    {&anim.new_card_enabled, "plugin:gloview:new_card_enabled", 1},
    {&anim.new_card_ms, "plugin:gloview:new_card_ms", -1},
    {&anim.swap_pulse_enabled, "plugin:gloview:swap_pulse_enabled", 0},
    {&anim.swap_pulse_ms, "plugin:gloview:swap_pulse_ms", 180},
    {&anim.strip_step_enabled, "plugin:gloview:strip_step_enabled", 1},
    {&anim.strip_step_ms, "plugin:gloview:strip_step_ms", 200},
    {&anim.populate_enabled, "plugin:gloview:populate_enabled", 1},
    {&anim.populate_ms, "plugin:gloview:populate_ms", 250},
    {&anim.drop_enabled, "plugin:gloview:drop_enabled", 1},
    {&anim.drop_ms, "plugin:gloview:drop_ms", 320},
    {&anim.ws_enter_enabled, "plugin:gloview:ws_enter_enabled", 1},
    {&anim.ws_enter_ms, "plugin:gloview:ws_enter_ms", 250},
    {&anim.ws_exit_enabled, "plugin:gloview:ws_exit_enabled", 1},
    {&anim.ws_exit_ms, "plugin:gloview:ws_exit_ms", 250},
    {&anim.swap_main_ms, "plugin:gloview:swap_main_ms", 320},
    {&anim.swap_partner_ms, "plugin:gloview:swap_partner_ms", 320},
    {&anim.expo_in_ms, "plugin:gloview:expo_in_ms", 250},
    {&anim.expo_out_ms, "plugin:gloview:expo_out_ms", 250},
    {&anim.drag_lift_enabled, "plugin:gloview:drag_lift_enabled", 1},
    {&anim.drag_lift_ms, "plugin:gloview:drag_lift_ms", 150},
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
    {&anim.grid_swap_anim, "plugin:gloview:grid_swap_anim", "horizontal"},
    {&anim.strip_swap_anim, "plugin:gloview:strip_swap_anim", "horizontal"},
    {&anim.ws_enter_anim, "plugin:gloview:ws_enter_anim", "slide"},
    {&anim.ws_exit_anim, "plugin:gloview:ws_exit_anim", "slide"},
    {&anim.ws_enter_curve, "plugin:gloview:ws_enter_curve", "easeout"},
    {&anim.ws_exit_curve, "plugin:gloview:ws_exit_curve", "easeout"},
    {&anim.swap_main_curve, "plugin:gloview:swap_main_curve", "easeinout"},
    {&anim.swap_partner_curve, "plugin:gloview:swap_partner_curve", "easeinout"},
    {&anim.expo_in_curve, "plugin:gloview:expo_in_curve", "easeout"},
    {&anim.expo_out_curve, "plugin:gloview:expo_out_curve", "easeout"},
    {&anim.drag_lift_curve, "plugin:gloview:drag_lift_curve", "easeout"},
    {&anim.open_curve, "plugin:gloview:open_curve", "easeout"},
    {&anim.close_curve, "plugin:gloview:close_curve", "easeout"},
    {&anim.reflow_curve, "plugin:gloview:reflow_curve", "easeout"},
    {&anim.new_card_curve, "plugin:gloview:new_card_curve", "back"},
    {&anim.swap_pulse_curve, "plugin:gloview:swap_pulse_curve", "back"},
    {&anim.strip_step_curve, "plugin:gloview:strip_step_curve", "easeinout"},
    {&anim.populate_curve, "plugin:gloview:populate_curve", "easeout"},
    {&anim.drop_curve, "plugin:gloview:drop_curve", "easeout"},
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
}

const Int *anim::leafEnabled(std::string_view name) {
  static const std::unordered_map<std::string_view, const Int *> k = {
      {"open", &open_enabled},         {"close", &close_enabled},
      {"reflow", &reflow_enabled},     {"new_card", &new_card_enabled},
      {"swap_pulse", &swap_pulse_enabled},
      {"strip_step", &strip_step_enabled}, {"populate", &populate_enabled},
      {"drop", &drop_enabled},
  };
  const auto it = k.find(name);
  return it == k.end() ? nullptr : it->second;
}

const Int *anim::leafMs(std::string_view name) {
  static const std::unordered_map<std::string_view, const Int *> k = {
      {"open", &open_ms},     {"close", &close_ms},
      {"reflow", &reflow_ms}, {"new_card", &new_card_ms},
      {"swap_pulse", &swap_pulse_ms}, {"strip_step", &strip_step_ms},
      {"populate", &populate_ms}, {"drop", &drop_ms},
      {"ws_in", &ws_enter_ms},          {"ws_out", &ws_exit_ms},
      {"swap_main", &swap_main_ms},     {"swap_partner", &swap_partner_ms},
      {"expo_in", &expo_in_ms},         {"expo_out", &expo_out_ms},
      {"drag_lift", &drag_lift_ms},
  };
  const auto it = k.find(name);
  return it == k.end() ? nullptr : it->second;
}

const Str *anim::leafCurve(std::string_view name) {
  static const std::unordered_map<std::string_view, const Str *> k = {
      {"open", &open_curve},     {"close", &close_curve},
      {"reflow", &reflow_curve}, {"new_card", &new_card_curve},
      {"swap_pulse", &swap_pulse_curve}, {"strip_step", &strip_step_curve},
      {"populate", &populate_curve}, {"drop", &drop_curve},
      {"ws_in", &ws_enter_curve},       {"ws_out", &ws_exit_curve},
      {"swap_main", &swap_main_curve},  {"swap_partner", &swap_partner_curve},
      {"expo_in", &expo_in_curve},      {"expo_out", &expo_out_curve},
      {"drag_lift", &drag_lift_curve},
  };
  const auto it = k.find(name);
  return it == k.end() ? nullptr : it->second;
}

} // namespace gloview::cfg
