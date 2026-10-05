#pragma once

// Live handles resolved through the V2 config system; adding one = a handle
// in its domain + a schema row in config.cpp + a README row.

#include <optional>
#include <string>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/helpers/Color.hpp>

#include "../layout.hpp"

struct lua_State;

namespace gloview::cfg {

class Int {
public:
  SP<Config::Values::CIntValue> v;
  int def = 0;
  int get() const { return v ? static_cast<int>(v->value()) : def; }
  operator int() const { return get(); }
};

class Float {
public:
  SP<Config::Values::CFloatValue> v;
  float def = 0.f;
  float get() const { return v ? static_cast<float>(v->value()) : def; }
  operator float() const { return get(); }
};

class Str {
public:
  SP<Config::Values::CStringValue> v;
  const char *def = "";
  std::string get() const { return v ? std::string(v->value()) : std::string(def); }
  operator std::string() const { return get(); }
};

// Parsed once, re-parsed only when the configured string changes; get(a) multiplies its alpha.
class Color {
public:
  SP<Config::Values::CStringValue> v;
  const char *def = "";
  CHyprColor get(float alphaMul = 1.0F) const;
};

struct grid {
  Int gap;
  Int padding;
  Int padding_top;
  Int padding_bottom;
  Float max_scale;
  Str layout;
};

inline grid grid;

// The run is a separate mechanism laid over whatever `layout` says, not a
// layout of its own — see Overview::layoutTiles for where it takes over.
struct carousel {
  Int enabled;
  Int cards;
  Int card_width;
  Float scale;
};

inline carousel carousel;

struct strip {
  Int height;
  Int offset;
  Int margin;
  Int gap;
  Int card_round;
  Str anchor;
  Str bar_position;
  Str empty_mode;
  Int all_card;
  Int wallpaper;
};

inline strip strip;

struct look {
  Int preview_round;
  Float preview_round_power;
  Int show_border;
  Int show_focus_border;
  Int border_size;
  Int hover_border_size;
  Int select_border_size;
  Float close_button_size;
  Float drag_size;
  Str close_button_position;
  Str close_button_visibility;
  Str close_trigger;
  Str close_button_icon;
};

inline look look;

struct colors {
  Color border;
  Color hover_border;
  Color select_border;
  Color shadow;
  Color backing;
  Color label;
  Color title_pill;
  Color drop_hint;
  Color close_button;
  Color close_glyph;
  Color strip_band;
  Color strip_card;
  Color strip_active;
  Color strip_active_border;
  Color strip_hover;
  Color strip_plus;
  Color strip_all;
};

inline colors colors;

struct blur {
  Float strength;
  Int passes;
  Int size;
  Int resolution;
  Color backdrop;
  Int fullscreen_background;
  Int frost_underlay;
};

inline blur blur;

// One LEAF per animated behavior: <leaf>_enabled, <leaf>_speed (multiplier over
// the base, >= 16ms), <leaf>_curve, plus <leaf>_style where types are selectable.
namespace anim {

struct Leaf {
  Int enabled;
  Float speed;
  Str curve;
};

inline Int enabled;
inline Leaf open{ {}, {}, {} };
inline Leaf close{ {}, {}, {} };
inline Leaf glide{ {}, {}, {} };
inline Leaf appear{ {}, {}, {} };
inline Leaf card{ {}, {}, {} };
inline Leaf pulse{ {}, {}, {} };
inline Leaf strip_step{ {}, {}, {} };
inline Leaf ws_enter{ {}, {}, {} };
inline Leaf ws_exit{ {}, {}, {} };
inline Leaf expo_in{ {}, {}, {} };
inline Leaf expo_out{ {}, {}, {} };
inline Leaf jump_in{ {}, {}, {} };
inline Leaf jump_out{ {}, {}, {} };
inline Leaf swap_main{ {}, {}, {} };
inline Leaf swap_partner{ {}, {}, {} };
inline Leaf drag{ {}, {}, {} };

inline Str ws_enter_style;
inline Str ws_exit_style;
inline Str jump_style;
inline Str grid_swap_style;
inline Str strip_swap_style;

}

struct AnimRule {
  std::optional<bool> enabled;
  std::optional<double> speed;
  std::optional<int> ms;
  std::optional<std::string> curve;
  std::optional<std::string> style;
};
void setAnimRule(const std::string &leaf, const AnimRule &rule);
const AnimRule *animRule(const std::string &leaf);
std::string animStyle(const std::string &thing);

struct LeafSpec {
  const char *name;
  anim::Leaf *leaf;
  float base;
  const char *curveDef;
};
const LeafSpec *animLeaf(const char *name);

int luaSetAnimRule(lua_State *L);

struct keys {
  Str close;
  Str next_workspace;
  Str prev_workspace;
  Str activate;
  Str close_window;
  Str left, right, up, down;
  Str desktop;
  Str all_workspaces;
  Str workspace;
  Str alt_tab_modifier;
  Int alt_tab_commit_on_release;
};

inline keys keys;

struct behavior {
  Str preview_mode;
  Str cursor_mode;
  Str workspace_key_mode;
  Str new_workspace_mode;
  Int focus_follows_mouse;
  Int scroll_switches_workspace;
  Int scroll_cards;
  Int passthrough_keys;
  Int exit_on_click;
  Int exit_on_switch;
  Int switch_on_drop;
  Int drag_to_swap;
  Int switch_on_new_workspace;
  Int show_all_workspaces;
  Int show_special;
  Int hold_lift_ms;
  Str tile_layout;
};

inline behavior behavior;

struct layer {
  Int hide_top;
  Int hide_overlay;
  Str above_namespaces;
};

inline layer layer;

struct debug {
  Int logs;
};

inline debug debug;

void registerAll(HANDLE handle);

}
