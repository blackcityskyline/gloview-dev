#pragma once

// The configuration schema — the single place that owns every plugin option
// (REFACTORING.md C1). Options are grouped by DOMAIN, mirroring the mental
// model: what does the option control? Each option is a typed live handle
// resolved through Hyprland's V2 config system (Lua `hl.config` and the ini
// frontend both work; edits apply without restart).
//
//   cfg::grid.gap            — read directly, implicit int conversion
//   cfg::colors.hover.get()  — CHyprColor, hex parsed once and cached
//   cfg::colors.hover.get(e) — alpha-multiplied (the old argb(col, e))
//
// WHY handles instead of string-keyed lookups: the old cfgInt("...") did an
// unordered_map<string> lookup — with a temporary std::string — at EVERY
// call, dozens of times per frame; cfgColor re-parsed the hex literal every
// frame on top. Handles turn both into a pointer dereference (plus a cached
// string compare for colors).
//
// Adding an option: one handle in its domain below + one row in the schema
// table in config.cpp + one README row. Nothing else.
//
// Registration happens once in PLUGIN_INIT (config.cpp registerAll), before
// any read is possible — the `def` members are only a last-resort fallback
// for reads before registration (which would be a bug anyway).

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

// Hex-literal color ("0xAARRGGBB", "#RRGGBB", or a palette-resolved string
// produced Lua-side). The literal is parsed once and re-parsed only when the
// configured string changes. get(alpha) multiplies the parsed alpha — the
// exact semantics of the old argb(parseHexColor(...), alpha).
class Color {
public:
  SP<Config::Values::CStringValue> v;
  const char *def = "";
  CHyprColor get(float alphaMul = 1.0F) const;
};

// ---- grid: main-area geometry ------------------------------------------------
struct grid {
  Int gap;                 // min spacing between tiles (px)
  Int padding;             // left/right outer margin
  Int padding_top;         // gap below the strip
  Int padding_bottom;      // bottom margin
  Float max_scale;         // never upscale past real*this
  Str layout;              // rows | grid | natural
};

inline grid grid;

// ---- strip: band geometry + contents ----------------------------------------
struct strip {
  Int height;              // band thickness (perpendicular to its edge)
  Int offset;              // inset from the anchored edge
  Int margin;
  Int gap;
  Int card_round;
  Str anchor;              // top|bottom|left|right ("" → bar_position)
  Str bar_position;        // deprecated alias for anchor
  Str empty_mode;          // show | neighbors | hide
  Int all_card;            // leading "All workspaces" card
  Int wallpaper;           // empty ws cards show a cover-fit wallpaper thumbnail
};

inline strip strip;

// ---- look: element appearance ------------------------------------------------
struct look {
  Int preview_round;       // corner radius for window previews (px)
  Float preview_round_power; // corner curve exponent (2 = circular)
  Int show_border;         // always-on base ring on every tile
  Int show_focus_border;   // hover/keyboard-selection ring on top
  Int border_size;
  Int hover_border_size;
  Int select_border_size;
  Float close_button_size; // scale over the computed base size
  Str close_button_position;
  Str close_button_visibility; // shift | always
  Str close_trigger;           // button | doubleclick
  Str close_button_icon;       // glyph drawn in the close buttons
};

inline look look;

// ---- colors ------------------------------------------------------------------
struct colors {
  Color border;        // base ring (show_border)
  Color hover_border;  // hovered tile ring
  Color select_border; // keyboard-selected tile ring
  Color shadow;
  Color backing;       // translucent-tile safety margin (alpha applied by renderer)
  Color label;         // window/workspace name text
  Color title_pill;    // desktop-mode pill behind a tile's label
  Color drop_hint;     // strip-card drop-zone highlight
  Color close_button;  // "✕" fill (per-window and per-workspace)
  Color close_glyph;   // the ✕ glyph itself
  Color strip_band;
  Color strip_card;
  Color strip_active;
  Color strip_active_border;
  Color strip_hover;
  Color strip_plus;
  Color strip_all;
};

inline colors colors;

// ---- blur / dim: the backdrop pipeline ---------------------------------------
struct blur {
  Float strength;          // 0..1 (0 = off)
  Int passes;              // custom gaussian iterations
  Int size;                // radius in screen px
  Int resolution;          // blur buffer = 1/N monitor res
  Color backdrop;          // the dim tint over the blur
  Int fullscreen_background; // fullscreen mpv becomes the backdrop (live)
  Int frost_underlay;      // DEPRECATED no-op (kept for old configs)
};

inline blur blur;

// ---- anim: master + leaves (see anim/clocks.cpp, anim/curves.cpp) ------------
struct anim {
  Int enabled;    // master switch: 0 = the whole plugin is instant
  Int duration;   // legacy shared knob (open/close/reflow follow it)

  Int open_enabled;
  Int open_ms;
  Str open_curve;
  Int close_enabled;
  Int close_ms;
  Str close_curve;
  Int reflow_enabled;
  Int reflow_ms;
  Str reflow_curve;
  Int new_card_enabled;
  Int new_card_ms;
  Str new_card_curve;
  Int swap_pulse_enabled;
  Int swap_pulse_ms;
  Str swap_pulse_curve;
  Int strip_step_enabled;
  Int strip_step_ms;
  Str strip_step_curve;
  Int populate_enabled;
  Int populate_ms;
  Str populate_curve;
  Int drop_enabled;
  Int drop_ms;
  Str drop_curve;
  Str grid_swap_anim;   // horizontal | slidevert | fade | pop
  Str strip_swap_anim;
  Str ws_enter_anim;    // ws-switch: pop | slide | slidevert | fade (incoming)
  Str ws_exit_anim;     // ws-switch: slide | slidevert | fade | pop (outgoing)

  // built-ins only; null if unknown
  const Int *leafEnabled(std::string_view name);
  const Int *leafMs(std::string_view name);
  const Str *leafCurve(std::string_view name);
};
inline anim anim;

// ---- keys --------------------------------------------------------------------
struct keys {
  Str close;               // dismiss
  Str next_workspace;
  Str prev_workspace;
  Str activate;
  Str close_window;        // send-close the selected tile's window
  Str left, right, up, down; // move selection
  Str desktop;             // flip canvas↔grid
  Str all_workspaces;
  Str workspace;           // number-row list, "1,2,3,..."
  Str alt_tab_modifier;    // alt | ctrl | shift | super
  Int alt_tab_commit_on_release;
};

inline keys keys;

// ---- behavior: logic switches --------------------------------------------------
struct behavior {
  Str preview_mode;        // live | snapshot
  Str cursor_mode;         // auto | software
  Str workspace_key_mode;  // switch | jump (digit keys)
  Str new_workspace_mode;  // fill | linear
  Int focus_follows_mouse;
  Int scroll_switches_workspace;
  Int passthrough_keys;    // unhandled keys reach Hyprland
  Int exit_on_click;
  Int exit_on_switch;
  Int switch_on_drop;
  Int drag_to_swap;
  Int switch_on_new_workspace;
  Int show_all_workspaces; // expo
  Int show_special;        // scratchpad as a strip card
};

inline behavior behavior;

// ---- layer: interaction with Hyprland's layer shell ----------------------------
struct layer {
  Int hide_top;            // fade out Top layers (bars) while open
  Int hide_overlay;        // fade out Overlay layers (popups)
  Str above_namespaces;    // namespaces drawn ABOVE the overview
};

inline layer layer;

// ---- debug ----------------------------------------------------------------------
struct debug {
  Int logs;                // verbose [gloview] logging → /tmp/gloview.log
};

inline debug debug;

// Register every option with Hyprland (once, from PLUGIN_INIT).
void registerAll(HANDLE handle);

} // namespace gloview::cfg
