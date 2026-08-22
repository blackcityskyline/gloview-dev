#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "overview.hpp"

inline HANDLE                             g_handle = nullptr;
inline std::unique_ptr<gloview::Overview> g_overviewOwned;

namespace {

// Keep the SP so cfg* can read the resolved value() — see ConfigRegistry in overview.hpp.
void addInt(const char* name, Config::INTEGER fallback) {
    auto v = makeShared<Config::Values::CIntValue>(name, "", fallback);
    HyprlandAPI::addConfigValueV2(g_handle, v);
    gloview::g_config.ints[name] = v;
}
void addFloat(const char* name, Config::FLOAT fallback) {
    auto v = makeShared<Config::Values::CFloatValue>(name, "", fallback);
    HyprlandAPI::addConfigValueV2(g_handle, v);
    gloview::g_config.floats[name] = v;
}
void addStr(const char* name, Config::STRING fallback) {
    auto v = makeShared<Config::Values::CStringValue>(name, "", fallback);
    HyprlandAPI::addConfigValueV2(g_handle, v);
    gloview::g_config.strings[name] = v;
}

// --- every plugin:gloview:* config value, grouped by concern ---
// Color options are plain strings too: either a manual "0xAARRGGBB" literal or
// a scheme-role keyword ("primary"/"secondary"/"error"/…) — ONE field does both
// jobs, see cfgColorScheme() in overview_core.cpp.

constexpr std::pair<const char*, Config::INTEGER> kIntCfg[] = {
    // --- layout / main area ---
    {"plugin:gloview:gap", 34},                    // min spacing between tiles (px)
    {"plugin:gloview:padding", 80},                // left/right outer margin (px)
    {"plugin:gloview:padding_top", 40},            // gap below the strip
    {"plugin:gloview:padding_bottom", 70},         // bottom margin
    {"plugin:gloview:duration", 360},              // open/close animation (ms)
    {"plugin:gloview:preview_round", 12},
    // deprecated, no longer read: strip window previews share preview_round /
    // preview_round_power (clamped to the card slot) — kept registered so
    // existing configs setting it don't error
    {"plugin:gloview:strip_preview_round", 4},
    {"plugin:gloview:blur_passes", 3},             // custom gaussian iterations for the backdrop blur (1..16)
    {"plugin:gloview:blur_size", 8},               // backdrop blur radius in screen px (1..200)
    {"plugin:gloview:blur_resolution", 4},         // backdrop blur buffer = 1/N monitor res (1..32; lower = sharper/cleaner)
    {"plugin:gloview:fullscreen_background", 0},   // 1: a fullscreen mpv window on the displayed workspace becomes the (blurred) backdrop instead of wallpaper (live video); 0 (default): the wallpaper shows and fullscreen windows stay hidden
    // --- workspace strip ---
    {"plugin:gloview:strip_height", 150},          // band thickness (perpendicular to its edge)
    {"plugin:gloview:strip_offset", 0},            // inset from the anchored edge (0 = flush, no gap)
    {"plugin:gloview:strip_margin", 22},
    {"plugin:gloview:strip_gap", 18},
    {"plugin:gloview:strip_card_round", 10},
    // --- input / keyboard navigation ---
    {"plugin:gloview:focus_follows_mouse", 1},       // keyboard selection tracks the hovered tile
    {"plugin:gloview:scroll_switches_workspace", 1}, // wheel over the main area steps prev/next workspace
    {"plugin:gloview:passthrough_keys", 1},          // unhandled keys reach Hyprland (keybinds keep working)
    {"plugin:gloview:exit_on_click", 1},             // click on empty space dismisses the overview
    {"plugin:gloview:debug_logs", 0},                // verbose [gloview] logging
    {"plugin:gloview:select_border_size", 3},        // keyboard-selected tile ring thickness (px); 0 disables the ring entirely
    {"plugin:gloview:hover_border_size", 3},         // hovered/focused tile ring thickness (px); 0 disables the ring entirely
    // Two independent, modular layers instead of one fixed look — combine freely:
    //   show_border:       an always-on base ring on EVERY tile (border_color/border_size)
    //   show_focus_border: the hover_border/select_border ring drawn on top on the
    //                      hovered/keyboard-selected tile (hover_border_size/select_border_size)
    // off+off = no borders; off+on = focus-only (the original look); on+off = a constant
    // ring; on+on = a constant ring that visibly changes on focus, since the focus ring
    // draws right over it.
    {"plugin:gloview:show_border", 0},
    {"plugin:gloview:show_focus_border", 1},
    {"plugin:gloview:border_size", 2},               // show_border ring thickness (px); 0 disables it
    {"plugin:gloview:alt_tab_commit_on_release", 1}, // 1 (default): releasing alt_tab_modifier focuses the selection & closes, like a normal alt-tab — 0: releasing does nothing, confirm with key_activate/click instead
    // --- workspace scope / strip contents ---
    {"plugin:gloview:show_all_workspaces", 0},        // main area shows every window on the monitor (expo), not just the displayed workspace
    {"plugin:gloview:show_special", 0},               // include the special (scratchpad) workspace as a strip card
    {"plugin:gloview:strip_all_card", 0},             // show a leading "All workspaces" card on the strip that toggles the expo view
    {"plugin:gloview:switch_on_drop", 0},             // dropping a window on a card also follows it to that workspace
    {"plugin:gloview:drag_to_swap", 1},               // dragging a preview onto another swaps the two windows' places
    {"plugin:gloview:exit_on_switch", 0},             // dismiss the overview when the live workspace changes underneath
    {"plugin:gloview:switch_on_new_workspace", 1},    // clicking "+" follows the display to the new workspace
    // --- bar / layer-shell hiding (waybar, quickshell-based bars, …) ---
    {"plugin:gloview:hide_top_layers", 0},            // fade out Top layer surfaces (bars) while the overview is up
    {"plugin:gloview:hide_overlay_layers", 0},        // fade out Overlay layer surfaces (popups/notifications)
};

constexpr std::pair<const char*, Config::FLOAT> kFloatCfg[] = {
    // --- layout / main area ---
    {"plugin:gloview:max_scale", 1.0F},           // never upscale past real*this
    {"plugin:gloview:preview_round_power", 2.0F}, // corner curve exponent for preview rounding (2 = circular; higher = squarer "squircle"); applied to the LIVE window surface too, so it now matches the chrome ring instead of showing square corners under a round border
    {"plugin:gloview:blur", 1.0F},                // backdrop/strip blur strength 0..1 (0 = off; floats allowed)
    // --- close buttons ---
    {"plugin:gloview:close_button_size", 1.0F},   // scale multiplier over the computed base button size
};

constexpr std::pair<const char*, const char*> kStrCfg[] = {
    // --- layout / main area ---
    {"plugin:gloview:layout", "rows"},      // rows | grid | natural
    // "live" (default) draws each preview from the window's LIVE surface tree
    // every frame — a playing video/animating page keeps the overview
    // recompositing at full refresh. "snapshot" captures each window's main
    // surface texture ONCE (at build) and renders that static texture instead:
    // zero per-frame live-surface work (measured ~20%→0% GPU idle on a Haswell
    // iGPU with a video tile), at the cost of a stale preview while content
    // changes under the overview.
    {"plugin:gloview:preview_mode", "live"},
    // auto (default): prefer Hyprland's hardware cursor plane when the driver exposes one (zero framebuffer writes, zero trails, zero GPU cost per move) — software: force a software cursor drawn by gloview with an opaque erase (use if you see stale cursor artifacts)
    {"plugin:gloview:cursor_mode", "auto"},
    // --- workspace strip ---
    {"plugin:gloview:anchor", ""},                  // top | bottom | left | right — which edge the strip attaches to (default top)
    {"plugin:gloview:bar_position", "top"},         // deprecated alias for anchor (top | bottom); used only if anchor is unset
    {"plugin:gloview:strip_empty_mode", "show"},    // show (default): every workspace, even empty — neighbors: occupied ones plus the displayed workspace's immediate empty numeric neighbors, cascading as you navigate into a run of them — hide: only occupied workspaces, no empty ones at all
    // --- colors ---
    {"plugin:gloview:backdrop_color", "0x73070a10"},    // dim + blur
    {"plugin:gloview:strip_band_color", "0x24ffffff"},  // faint top band
    {"plugin:gloview:strip_card_color", "0x3a0e131c"},
    {"plugin:gloview:strip_active_color", "0x4d1c2c44"},
    {"plugin:gloview:strip_active_border", "0xf0ffffff"},
    {"plugin:gloview:strip_hover_border", "0x80ffffff"},
    {"plugin:gloview:strip_plus_color", "0xd0eef4ff"},  // the "+" glyph only
    {"plugin:gloview:strip_all_color", "0xd0eef4ff"},   // the all-workspaces 2x2 glyph — own key, no longer tied to strip_plus_color
    {"plugin:gloview:shadow_color", "0x70000000"},
    {"plugin:gloview:hover_border", "0xf0ffffff"},
    {"plugin:gloview:border_color", "0x50ffffff"},      // the always-on base ring (show_border)
    {"plugin:gloview:select_border", "0xf066ccff"},     // keyboard-selected tile ring (distinct from hover)
    // --- keybinds (key names: esc/tab/enter/left/right/up/down/shift/hjkl/f1…/super/ctrl/alt/grave;
    //     a bare digit = that number-row key; comma/space separated; modifier combos as
    //     "shift+tab" / "ctrl+shift+k"; "" disables → key falls through) ---
    {"plugin:gloview:key_close", "escape"},              // dismiss (tab now cycles workspaces; add it back here to restore the old behavior)
    {"plugin:gloview:key_next_workspace", "tab"},        // cycle the displayed workspace forward (wraps); "" disables
    {"plugin:gloview:key_prev_workspace", "shift+tab"},  // …backward
    {"plugin:gloview:key_activate", "enter"},            // focus the selected tile
    {"plugin:gloview:key_close_window", "d"},            // send-close the selected tile's window (stays open, reflows); "" disables
    {"plugin:gloview:key_left", "left"},                 // move selection
    {"plugin:gloview:key_right", "right"},
    {"plugin:gloview:key_up", "up"},
    {"plugin:gloview:key_down", "down"},
    {"plugin:gloview:key_desktop", "shift"},             // flip canvas<->grid
    {"plugin:gloview:key_all_workspaces", "a"},          // toggle the all-workspaces (expo) view; "" disables
    {"plugin:gloview:key_workspace", "1,2,3,4,5,6,7,8,9,0"}, // key at position N (0-based) switches DIRECTLY to workspace N+1 (so "0" is always workspace 10) — creates it first if it doesn't exist yet, same as clicking "+"; independent of what's currently on the strip
    // switch (default): a digit changes the DISPLAYED workspace and the overview stays open (Ctrl+digit is a no-op) — jump: a digit switches AND immediately closes the overview, landing you there with no Enter needed; hold Ctrl+digit in this mode for the old switch-and-stay behavior
    {"plugin:gloview:key_workspace_mode", "switch"},
    // Alt-Tab is triggered by the gloview:alttab / gloview:alttabback DISPATCHERS, not a raw
    // key combo here — bind them to whatever modifier+key you like (typically the same one
    // you'd bind gloview:allworkspaces to), e.g.:  bind = SUPER, TAB, gloview:alttab
    // alt_tab_modifier says WHICH modifier's release commits the selection (see
    // alt_tab_commit_on_release) — set it to match whatever modifier is in that bind.
    {"plugin:gloview:alt_tab_modifier", "alt"},          // alt | ctrl | shift | super — which modifier's release commits, when alt_tab_commit_on_release is on
    // --- workspace scope / strip contents ---
    {"plugin:gloview:new_workspace_mode", "fill"},       // fill (default): "+" takes the lowest free workspace id (backfills a gap) — linear: always appends past the highest existing id
    // --- close buttons ---
    {"plugin:gloview:close_button_color", "0xe6e23b3b"}, // "✕" close button fill (both per-window and per-workspace); try "error" if group:col.border_locked_active is themed
    {"plugin:gloview:close_button_visibility", "shift"}, // shift (default, "standard"): close buttons only show in desktop/canvas mode (key_desktop) — always: show them on every tile and strip card all the time
    {"plugin:gloview:close_button_icon", "✕"},           // glyph drawn in the close buttons
    {"plugin:gloview:close_button_position", "top-right"}, // top-right | top-left | bottom-right | bottom-left
    // button (default): the "✕" close button (close_button_visibility) — doubleclick:
    // double-click/double-tap directly on a tile closes it instead (the "✕" is hidden);
    // keyboard key_close_window and the strip card's whole-workspace "✕"/middle-click are
    // unaffected either way
    {"plugin:gloview:close_trigger", "button"},
    // --- bar / layer-shell hiding (waybar, quickshell-based bars, …) ---
    {"plugin:gloview:above_namespaces", ""},             // comma/space list of layer namespaces to draw ABOVE the overview (supports trailing '*' glob); namespaces containing "aboveoverview" are always treated this way
};

// One table drives ALL three invoke paths (dispatcher / hyprctl / Lua). Adding
// an action = adding one entry here; the three registration loops below pick up
// whichever paths its three name fields enable.
//
// hyprctl notes:
//   gloview        exact command — reliable invoke path:  hyprctl gloview
//   gloviewclose   close-only (no-op if not open): dismiss the overlay before
//                  unloading. Unloading mid-render with the overview up tears
//                  down the render hooks while an in-flight frame still
//                  references them → Hyprland crash.
//   gloviewunload  UNLOAD-safe teardown (run by the `reload` target before
//                  `plugin unload`). Unlike gloviewclose (which only *starts*
//                  the close animation), this drops all overlay state + the
//                  recapture timer synchronously, so the next frame renders
//                  with no plugin-owned pass elements and dlclose can't free a
//                  callback still referenced mid-frame. Makes reload
//                  deterministic.
struct Action {
    const char* dispatch; // suffix of the "gloview:<suffix>" dispatcher; nullptr = none
    const char* hyprctl;  // exact hyprctl command; nullptr = none
    const char* lua;      // function registered under gloview.*; nullptr = none
    void (*invoke)();     // nullary; checks g_overview itself
};

constexpr std::array<Action, 8> kActions{{
    {"toggle", "gloview", "toggle",
     [] { if (g_overview) g_overview->toggle(); }},
    {"open", nullptr, "open",
     [] { if (g_overview) g_overview->open(); }},
    {"close", "gloviewclose", "close",
     [] { if (g_overview) g_overview->close(); }},
    {"desktop", "gloviewdesktop", "desktop",
     [] { if (g_overview) g_overview->toggleDesktop(); }}, // toggle the free-arrange desktop mode
    {"allworkspaces", "gloviewall", "allworkspaces",
     [] { if (g_overview) g_overview->toggleAllWorkspaces(); }}, // open/toggle the all-workspaces expo view
    // alt-tab cycling — see the alt_tab_* config keys
    {"alttab", "gloviewalttab", "alttab",
     [] { if (g_overview) g_overview->altTabInvoke(false); }},
    {"alttabback", "gloviewalttabback", "alttabback",
     [] { if (g_overview) g_overview->altTabInvoke(true); }},
    {nullptr, "gloviewunload", nullptr,
     [] { if (g_overview) g_overview->hardClose(); }},
}};

// addLuaFunction wants a raw int(*)(lua_State*) — no captures — so each Lua
// route goes through a stateless thunk indexed like the table.
template <size_t I>
int luaInvoke(lua_State*) {
    kActions[I].invoke();
    return 0;
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    for (const auto& [name, def] : kIntCfg)
        addInt(name, def);
    for (const auto& [name, def] : kFloatCfg)
        addFloat(name, def);
    for (const auto& [name, def] : kStrCfg)
        addStr(name, def);

    g_overviewOwned = std::make_unique<gloview::Overview>(handle);
    g_overview      = g_overviewOwned.get();
    if (!g_overview->initialize()) {
        HyprlandAPI::addNotification(handle, "[gloview] initialization failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
        g_overview = nullptr;
        g_overviewOwned.reset();
        // Throw so Hyprland ejects the plugin (it catches this and runs unloadPlugin).
        // Returning normally instead kept a half-alive instance loaded — dispatchers and
        // config registered but no render hooks — which then blocked every later load
        // attempt (two instances can't hook shouldRenderWindow twice).
        throw std::runtime_error("[gloview] initialization failed");
    }

    const bool isLua = Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA;

    [&]<size_t... Is>(std::index_sequence<Is...>) {
        ((kActions[Is].dispatch &&
          HyprlandAPI::addDispatcherV2(
              handle, std::string("gloview:") + kActions[Is].dispatch,
              [fn = kActions[Is].invoke](std::string) -> SDispatchResult {
                  fn();
                  return {.success = true};
              })),
         ...);

        ((kActions[Is].hyprctl &&
          HyprlandAPI::registerHyprCtlCommand(
              handle,
              SHyprCtlCommand{
                  .name  = kActions[Is].hyprctl,
                  .exact = true,
                  .fn    = [fn = kActions[Is].invoke](eHyprCtlOutputFormat, std::string) -> std::string {
                      fn();
                      return "ok\n";
                  }})),
         ...);

        if (isLua)
            ((kActions[Is].lua &&
              HyprlandAPI::addLuaFunction(handle, "gloview", kActions[Is].lua,
                                          &luaInvoke<Is>)),
             ...);
    }(std::make_index_sequence<kActions.size()>{});

    HyprlandAPI::reloadConfig();

    return {
        .name        = "GloView",
        .description = "macOS Mission Control-style overview",
        .author      = "Vergil",
        .version     = "0.3.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_overview = nullptr;
    g_overviewOwned.reset();
}
