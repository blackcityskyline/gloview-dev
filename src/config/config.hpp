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
  // Depth shading of the run: the nearest unfocused card's tone, the far one, and how fast
  // each step deeper multiplies down.
  Float tone_near;
  Float tone_far;
  Float depth_taper;
  Int cards;
  Int card_width;
  Float scale;
  // Wheel paging: notches * speed = cards moved. 1 = one card per notch,
  // 0.5 = one card every two notches, 3 = three per notch.
  Float scroll_speed;
  // Carousel state when the all-workspaces view opens. The key binding is always
  // a toggle; this only decides where it starts.
  Int on_open;
};

inline carousel carousel;

struct strip {
  Int height;
  Int offset;
  Int margin;
  Int gap;
  Int card_round;
  // Geometry that was literal in the strip code: the label strip under a card, how far a
  // card widens to fit its label, how much of the strip's thickness the slide-out covers,
  // and the hard ceiling on offset as a fraction of the cross axis.
  Float label_band;
  Float label_extra;
  Float slide_frac;
  Float offset_max;
  Str anchor;
  Str bar_position;
  Str empty_mode;
  Int all_card;
  Int wallpaper;
  // Percent. The band's OWN alpha comes from here and nowhere else -- strip_band_color
  // supplies RGB only. Two sources of alpha made its density depend on which string was
  // configured, and how it read depended on what was behind it.
  Int band_opacity;
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
  // Tile chrome geometry that was literal in tile_view.cpp: shadow blur at rest and lifted,
  // shadow alpha multiplier, title-pill padding and the gaps around it, and how much of the
  // close button the glyph fills.
  Float shadow_radius;
  Float shadow_radius_lift;
  Float shadow_alpha;
  Float title_pad_x;
  Float title_pad_y;
  Float title_gap;
  Float title_margin;
  Float close_glyph_fill;
  // Drag preview offset from the pointer, so the drop zones stay visible under it, and the
  // vertical lift of a SlideVert swap.
  Float drag_offset_x;
  Float drag_offset_y;
  Float swap_lift;
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
  Color label;
  Color title_pill;
  // Registered, never read. The translucent-tile backing fill it configured is gone
  // (0725444), but Hyprland REJECTS a config file that sets an unknown plugin key, so
  // existing configs like this one must keep loading.
  Color backing;  Color carousel_count_pill;
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
};

inline blur blur;

// One LEAF per animated behavior: <leaf>_enabled, <leaf>_speed (multiplier over
// the base, >= 16ms), <leaf>_curve, plus <leaf>_style where types are selectable.


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
  Str carousel;
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
  Int passthrough_keys;
  Int exit_on_click;
  // Closing a window by dragging it to the screen edge. The zone is the strip opposite the
  // strip band; disabling it leaves the edge a no-op instead of a trap.
  Int kill_zone;
  Float kill_zone_size;
  Int kill_zone_reach;
  Float kill_zone_falloff;
  Int click_window_ms;
  Int exit_on_switch;
  Int switch_on_drop;
  Int drag_to_swap;
  Int switch_on_new_workspace;
  Int show_all_workspaces;
  Int show_special;
  Int alttab_advances; // the entry is aimed at the next window
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
