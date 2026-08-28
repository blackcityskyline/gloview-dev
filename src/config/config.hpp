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
  Float drag_size;         // drag-phase preview as a fraction of its slot
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

// ---- anim: master + declarative leaves --------------------------------------
// One LEAF per animated behavior; every leaf reads exactly
//   <leaf>_enabled (0/1), <leaf>_speed (float multiplier over the leaf's
//   built-in base duration, clamped to >= 16ms), <leaf>_curve (a registry
//   name — native, Lua function or Lua bezier),
// plus <leaf>_style where the motion has selectable types. The bases live in
// one table in config.cpp — the single place that documents what a speed of
// 1.0 means for every leaf.
namespace anim {

struct Leaf {
  Int enabled;
  Float speed;
  Str curve;
};

inline Int enabled; // master: 0 = the whole plugin is instant
inline Leaf open{ {}, {}, {} };        // overview entry reveal
inline Leaf close{ {}, {}, {} };       // exit collapse
inline Leaf glide{ {}, {}, {} };       // tile movement slot <-> slot
inline Leaf appear{ {}, {}, {} };      // staggered entry of brand-new tiles
inline Leaf card{ {}, {}, {} };        // "+" card pop-in
inline Leaf pulse{ {}, {}, {} };       // success ring after swaps/moves
inline Leaf strip_step{ {}, {}, {} };  // animated strip scroll
inline Leaf ws_enter{ {}, {}, {} };    // ws switch: incoming tiles
inline Leaf ws_exit{ {}, {}, {} };     // ws switch: outgoing ghosts
inline Leaf expo_in{ {}, {}, {} };     // all<->one flip: spread
inline Leaf expo_out{ {}, {}, {} };    // all<->one flip: collapse
inline Leaf jump_in{ {}, {}, {} };     // jump mode: incoming tiles (after instant switch)
inline Leaf jump_out{ {}, {}, {} };    // jump mode: outgoing close glide
inline Leaf swap_main{ {}, {}, {} };   // swap exchange: initiator flight
inline Leaf swap_partner{ {}, {}, {} };// swap exchange: partner flight
inline Leaf drag{ {}, {}, {} };        // grab pickup AND release return home

// Motion TYPE selectors (<thing>_style keys).
inline Str ws_enter_style;  // pop | slide | slidevert | fade (incoming)
inline Str ws_exit_style;   // slide | slidevert | fade | pop (outgoing)
inline Str jump_style;      // instant | slide | slidevert | fade | pop
                             // instant: mute the ws-slide, go straight to close
                             // others:  play that style first, then close
inline Str grid_swap_style; // horizontal | slidevert | fade | pop
inline Str strip_swap_style;

} // namespace anim

// Imperative rules layered OVER the config keys (gloview.animation{...}):
// whatever a rule sets wins; unset fields fall through to the schema.
struct AnimRule {
  std::optional<bool> enabled;
  std::optional<double> speed;
  std::optional<int> ms; // absolute window, beats speed
  std::optional<std::string> curve;
  std::optional<std::string> style;
};
void setAnimRule(const std::string &leaf, const AnimRule &rule);
const AnimRule *animRule(const std::string &leaf);
// Style of a <thing>_style key with its rule override applied.
std::string animStyle(const std::string &thing);

// Leaf lookup for the resolver: name -> handles + built-in base duration
// (what a speed of 1.0 means).
struct LeafSpec {
  const char *name;
  anim::Leaf *leaf;
  float base;       // ms at speed 1.0
  const char *curveDef;
};
const LeafSpec *animLeaf(const char *name);

// gloview.animation({ leaf = "...", enabled = bool, speed = n, ms = n,
//                     curve = "...", style = "..." }) — Lua side.
int luaSetAnimRule(lua_State *L);

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
  Int hold_lift_ms;        // ms to hold before lift animation triggers
  // tile_layout: how windows are arranged on drop.
  // "none"    — legacy swap/move (default)
  // "dwindle" — use assignToSpace(focalPoint) for a dwindle half-split
  // future:     "bsp", "master", "columns"
  Str tile_layout;
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
