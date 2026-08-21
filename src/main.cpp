#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>

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

inline HANDLE                         g_handle = nullptr;
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
// Color options (a manual "0xAARRGGBB" literal OR a scheme-role keyword like
// "primary"/"secondary"/"error", see cfgColorScheme() in overview_core.cpp)
// are plain strings too — this alias just documents intent at each call site
// below instead of every color option reading like a generic addStr().
inline void addColor(const char* name, const char* fallbackHex) {
    addStr(name, fallbackHex);
}

SDispatchResult dispToggle(std::string) {
    if (g_overview)
        g_overview->toggle();
    return {.success = true};
}
SDispatchResult dispOpen(std::string) {
    if (g_overview)
        g_overview->open();
    return {.success = true};
}
SDispatchResult dispClose(std::string) {
    if (g_overview)
        g_overview->close();
    return {.success = true};
}
SDispatchResult dispDesktop(std::string) {
    if (g_overview)
        g_overview->toggleDesktop();
    return {.success = true};
}
SDispatchResult dispAllWorkspaces(std::string) {
    if (g_overview)
        g_overview->toggleAllWorkspaces();
    return {.success = true};
}
SDispatchResult dispAltTab(std::string) {
    if (g_overview)
        g_overview->altTabInvoke(false);
    return {.success = true};
}
SDispatchResult dispAltTabBack(std::string) {
    if (g_overview)
        g_overview->altTabInvoke(true);
    return {.success = true};
}

int luaToggle(lua_State*) {
    if (g_overview)
        g_overview->toggle();
    return 0;
}
int luaOpen(lua_State*) {
    if (g_overview)
        g_overview->open();
    return 0;
}
int luaClose(lua_State*) {
    if (g_overview)
        g_overview->close();
    return 0;
}
int luaDesktop(lua_State*) {
    if (g_overview)
        g_overview->toggleDesktop();
    return 0;
}
int luaAllWorkspaces(lua_State*) {
    if (g_overview)
        g_overview->toggleAllWorkspaces();
    return 0;
}
int luaAltTab(lua_State*) {
    if (g_overview)
        g_overview->altTabInvoke(false);
    return 0;
}
int luaAltTabBack(lua_State*) {
    if (g_overview)
        g_overview->altTabInvoke(true);
    return 0;
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    // --- layout / main area ---
    addStr("plugin:gloview:layout", "rows");              // rows | grid | natural
    addInt("plugin:gloview:gap", Config::INTEGER{34});    // min spacing between tiles (px)
    addInt("plugin:gloview:padding", Config::INTEGER{80}); // left/right outer margin (px)
    addInt("plugin:gloview:padding_top", Config::INTEGER{40});    // gap below the strip
    addInt("plugin:gloview:padding_bottom", Config::INTEGER{70}); // bottom margin
    addFloat("plugin:gloview:max_scale", Config::FLOAT{1.0F});    // never upscale past real*this
    addInt("plugin:gloview:duration", Config::INTEGER{360});      // open/close animation (ms)
    addInt("plugin:gloview:preview_round", Config::INTEGER{12});
    addFloat("plugin:gloview:preview_round_power", Config::FLOAT{2.0F}); // corner curve exponent for preview rounding (2 = circular; higher = squarer "squircle"); applied to the LIVE window surface too, so it now matches the chrome ring instead of showing square corners under a round border
    addInt("plugin:gloview:strip_preview_round", Config::INTEGER{4});   // deprecated, no longer read: strip window previews now share preview_round/preview_round_power (clamped to the card slot) — kept registered so existing configs setting it don't error
    addFloat("plugin:gloview:blur", Config::FLOAT{1.0F});         // backdrop/strip blur strength 0..1 (0 = off; floats allowed)
    addInt("plugin:gloview:blur_passes", Config::INTEGER{3});     // custom gaussian iterations for the backdrop blur (1..16)
    addInt("plugin:gloview:blur_size", Config::INTEGER{8});       // backdrop blur radius in screen px (1..200)
    addInt("plugin:gloview:blur_resolution", Config::INTEGER{4}); // backdrop blur buffer = 1/N monitor res (1..32; lower = sharper/cleaner)
    addInt("plugin:gloview:fullscreen_background", Config::INTEGER{0}); // 1: a fullscreen mpv window on the displayed workspace becomes the (blurred) backdrop instead of wallpaper (live video); 0 (default): the wallpaper shows and fullscreen windows stay hidden
    // "live" (default) draws each preview from the window's LIVE surface tree
    // every frame — a playing video/animating page keeps the overview
    // recompositing at full refresh. "snapshot" captures each window's main
    // surface texture ONCE (at build) and renders that static texture instead:
    // zero per-frame live-surface work (measured ~20%→0% GPU idle on a Haswell
    // iGPU with a video tile), at the cost of a stale preview while content
    // changes under the overview.
    addStr("plugin:gloview:preview_mode", "live");

    addStr("plugin:gloview:cursor_mode", "auto");         // auto (default): prefer Hyprland's hardware cursor plane when the driver exposes one (zero framebuffer writes, zero trails, zero GPU cost per move) — software: force a software cursor drawn by gloview with an opaque erase (use if you see stale cursor artifacts)

    // --- workspace strip ---
    addStr("plugin:gloview:anchor", "");                          // top | bottom | left | right — which edge the strip attaches to (default top)
    addStr("plugin:gloview:bar_position", "top");                 // deprecated alias for anchor (top | bottom); used only if anchor is unset
    addInt("plugin:gloview:strip_height", Config::INTEGER{150});  // band thickness (perpendicular to its edge)
    addInt("plugin:gloview:strip_offset", Config::INTEGER{0});    // inset from the anchored edge (0 = flush, no gap)
    addInt("plugin:gloview:strip_margin", Config::INTEGER{22});
    addInt("plugin:gloview:strip_gap", Config::INTEGER{18});
    addInt("plugin:gloview:strip_card_round", Config::INTEGER{10});

    // --- colors ---
    // ONE field per color: either a manual "0xAARRGGBB" (also accepts "0xRRGGBB" /
    // "#AARRGGBB" / "#RRGGBB" / bare hex), or a scheme-role keyword — "primary" |
    // "secondary" | "error"/"danger" | "group_active" | "group_inactive" — which pulls RGB
    // from Hyprland's OWN live general:col.*/group:col.* (whatever a theming tool — a
    // Noctalia/matugen palette, pywal, wallust, or a hand-edited hyprland.conf/lua — has
    // actually set those to, since gloview reads them back through Hyprland's own config
    // system after all variable substitution has already happened) while keeping the alpha
    // written here (or an explicit "role:AA" override). See cfgColorScheme()/schemeGradient()
    // in overview_core.cpp for the full grammar and the reasoning behind the whitelist.
    addColor("plugin:gloview:backdrop_color", "0x73070a10");       // dim + blur
    addColor("plugin:gloview:strip_band_color", "0x24ffffff");     // faint top band
    addColor("plugin:gloview:strip_card_color", "0x3a0e131c");
    addColor("plugin:gloview:strip_active_color", "0x4d1c2c44");
    addColor("plugin:gloview:strip_active_border", "0xf0ffffff");
    addColor("plugin:gloview:strip_hover_border", "0x80ffffff");
    addColor("plugin:gloview:strip_plus_color", "0xd0eef4ff");     // the "+" glyph only
    addColor("plugin:gloview:strip_all_color", "0xd0eef4ff");      // the all-workspaces 2x2 glyph — own key, no longer tied to strip_plus_color
    addColor("plugin:gloview:shadow_color", "0x70000000");
    addColor("plugin:gloview:hover_border", "0xf0ffffff");
    addColor("plugin:gloview:border_color", "0x50ffffff");         // the always-on base ring (show_border)

    // --- input / keyboard navigation ---
    addInt("plugin:gloview:focus_follows_mouse", Config::INTEGER{1});      // keyboard selection tracks the hovered tile
    addInt("plugin:gloview:scroll_switches_workspace", Config::INTEGER{1});// wheel over the main area steps prev/next workspace
    addInt("plugin:gloview:passthrough_keys", Config::INTEGER{1});         // unhandled keys reach Hyprland (keybinds keep working)
    addInt("plugin:gloview:exit_on_click", Config::INTEGER{1});           // click on empty space dismisses the overview
    addInt("plugin:gloview:debug_logs", Config::INTEGER{0});              // verbose [gloview] logging
    addInt("plugin:gloview:select_border_size", Config::INTEGER{3});      // keyboard-selected tile ring thickness (px); 0 disables the ring entirely
    addColor("plugin:gloview:select_border", "0xf066ccff"); // keyboard-selected tile ring (distinct from hover)
    addInt("plugin:gloview:hover_border_size", Config::INTEGER{3});        // hovered/focused tile ring thickness (px); 0 disables the ring entirely
    // Two independent, modular layers instead of one fixed look — combine freely:
    //   show_border:       an always-on base ring on EVERY tile (border_color/border_size)
    //   show_focus_border: the hover_border/select_border ring drawn on top on the
    //                      hovered/keyboard-selected tile (hover_border_size/select_border_size)
    // off+off = no borders; off+on = focus-only (the original look); on+off = a constant
    // ring; on+on = a constant ring that visibly changes on focus, since the focus ring
    // draws right over it.
    addInt("plugin:gloview:show_border", Config::INTEGER{0});
    addInt("plugin:gloview:show_focus_border", Config::INTEGER{1});
    addInt("plugin:gloview:border_size", Config::INTEGER{2});             // show_border ring thickness (px); 0 disables it

    // --- keybinds (key names: esc/tab/enter/left/right/up/down/shift/hjkl/f1…/super/ctrl/alt/grave;
    //     a bare digit = that number-row key; comma/space separated; modifier combos as
    //     "shift+tab" / "ctrl+shift+k"; "" disables → key falls through) ---
    addStr("plugin:gloview:key_close", "escape");              // dismiss (tab now cycles workspaces; add it back here to restore the old behavior)
    addStr("plugin:gloview:key_next_workspace", "tab");        // cycle the displayed workspace forward (wraps); "" disables
    addStr("plugin:gloview:key_prev_workspace", "shift+tab");  // …backward
    addStr("plugin:gloview:key_activate", "enter");       // focus the selected tile
    addStr("plugin:gloview:key_close_window", "d");       // send-close the selected tile's window (stays open, reflows); "" disables
    addStr("plugin:gloview:key_left", "left");            // move selection
    addStr("plugin:gloview:key_right", "right");
    addStr("plugin:gloview:key_up", "up");
    addStr("plugin:gloview:key_down", "down");
    addStr("plugin:gloview:key_desktop", "shift");        // flip canvas<->grid
    addStr("plugin:gloview:key_all_workspaces", "a");     // toggle the all-workspaces (expo) view; "" disables
    addStr("plugin:gloview:key_workspace", "1,2,3,4,5,6,7,8,9,0"); // key at position N (0-based) switches DIRECTLY to workspace N+1 (so "0" is always workspace 10) — creates it first if it doesn't exist yet, same as clicking "+"; independent of what's currently on the strip
    addStr("plugin:gloview:key_workspace_mode", "switch");  // switch (default): a digit changes the DISPLAYED workspace and the overview stays open (Ctrl+digit is a no-op) — jump: a digit switches AND immediately closes the overview, landing you there with no Enter needed; hold Ctrl+digit in this mode for the old switch-and-stay behavior
    // Alt-Tab is triggered by the gloview:alttab / gloview:alttabback DISPATCHERS, not a raw
    // key combo here — bind them to whatever modifier+key you like (typically the same one
    // you'd bind gloview:allworkspaces to), e.g.:  bind = SUPER, TAB, gloview:alttab
    // alt_tab_modifier says WHICH modifier's release commits the selection (see
    // alt_tab_commit_on_release) — set it to match whatever modifier is in that bind.
    addStr("plugin:gloview:alt_tab_modifier", "alt");            // alt | ctrl | shift | super — which modifier's release commits, when alt_tab_commit_on_release is on
    addInt("plugin:gloview:alt_tab_commit_on_release", Config::INTEGER{1}); // 1 (default): releasing alt_tab_modifier focuses the selection & closes, like a normal alt-tab — 0: releasing does nothing, confirm with key_activate/click instead
    addStr("plugin:gloview:alt_tab_mode", "smart");   // smart (default): first hop lands on the most-recently-focused window (Hyprland's own system-wide focus history), then walks back through recency; linear: simple fixed circular order

    // --- workspace scope / strip contents ---
    addInt("plugin:gloview:show_all_workspaces", Config::INTEGER{0}); // main area shows every window on the monitor (expo), not just the displayed workspace
    addStr("plugin:gloview:strip_empty_mode", "show"); // show (default): every workspace, even empty — neighbors: occupied ones plus the displayed workspace's immediate empty numeric neighbors, cascading as you navigate into a run of them — hide: only occupied workspaces, no empty ones at all
    addInt("plugin:gloview:show_special", Config::INTEGER{0});        // include the special (scratchpad) workspace as a strip card
    addInt("plugin:gloview:strip_all_card", Config::INTEGER{0});      // show a leading "All workspaces" card on the strip that toggles the expo view
    addInt("plugin:gloview:switch_on_drop", Config::INTEGER{0});      // dropping a window on a card also follows it to that workspace
    addInt("plugin:gloview:drag_to_swap", Config::INTEGER{1});        // dragging a preview onto another swaps the two windows' places
    addInt("plugin:gloview:exit_on_switch", Config::INTEGER{0});      // dismiss the overview when the live workspace changes underneath
    addInt("plugin:gloview:switch_on_new_workspace", Config::INTEGER{1}); // clicking "+" follows the display to the new workspace
    addStr("plugin:gloview:new_workspace_mode", "fill"); // fill (default): "+" takes the lowest free workspace id (backfills a gap) — linear: always appends past the highest existing id
    addColor("plugin:gloview:close_button_color", "0xe6e23b3b"); // "✕" close button fill (both per-window and per-workspace); try "error" if group:col.border_locked_active is themed
    addStr("plugin:gloview:close_button_visibility", "shift");  // shift (default, "standard"): close buttons only show in desktop/canvas mode (key_desktop) — always: show them on every tile and strip card all the time
    addStr("plugin:gloview:close_button_icon", "✕");            // glyph drawn in the close buttons
    addFloat("plugin:gloview:close_button_size", Config::FLOAT{1.0F}); // scale multiplier over the computed base button size
    addStr("plugin:gloview:close_button_position", "top-right"); // top-right | top-left | bottom-right | bottom-left
    addStr("plugin:gloview:close_trigger", "button"); // button (default): the "✕" close button (close_button_visibility) — doubleclick: double-click/double-tap directly on a tile closes it instead (the "✕" is hidden); keyboard key_close_window and the strip card's whole-workspace "✕"/middle-click are unaffected either way

    // --- bar / layer-shell hiding (waybar, quickshell-based bars, …) ---
    addInt("plugin:gloview:hide_top_layers", Config::INTEGER{0});     // fade out Top layer surfaces (bars) while the overview is up
    addInt("plugin:gloview:hide_overlay_layers", Config::INTEGER{0}); // fade out Overlay layer surfaces (popups/notifications)
    addStr("plugin:gloview:above_namespaces", "");                    // comma/space list of layer namespaces to draw ABOVE the overview (supports trailing '*' glob); namespaces containing "aboveoverview" are always treated this way

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

    HyprlandAPI::addDispatcherV2(handle, "gloview:toggle", dispToggle);
    HyprlandAPI::addDispatcherV2(handle, "gloview:open", dispOpen);
    HyprlandAPI::addDispatcherV2(handle, "gloview:close", dispClose);
    HyprlandAPI::addDispatcherV2(handle, "gloview:desktop", dispDesktop); // toggle the free-arrange desktop mode
    HyprlandAPI::addDispatcherV2(handle, "gloview:allworkspaces", dispAllWorkspaces); // open/toggle the all-workspaces expo view
    HyprlandAPI::addDispatcherV2(handle, "gloview:alttab", dispAltTab);         // alt-tab cycling — see the alt_tab_* config keys
    HyprlandAPI::addDispatcherV2(handle, "gloview:alttabback", dispAltTabBack); // …backward

    // hyprctl command (exact, not lua-evaluated) — reliable invoke path:  hyprctl gloview
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloview",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->toggle();
                                                        return "ok\n";
                                                    },
                                                });

    // close-only (no-op if not open): dismiss the overlay before unloading.
    // Unloading mid-render with the overview up tears down the render hooks while
    // an in-flight frame still references them → Hyprland crash.
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewclose",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->close();
                                                        return "ok\n";
                                                    },
                                                });

    // UNLOAD-safe teardown:  hyprctl gloviewunload  — run by the `reload` target
    // before `plugin unload`. Unlike gloviewclose (which only *starts* the close
    // animation), this drops all overlay state + the recapture timer synchronously,
    // so the next frame renders with no plugin-owned pass elements and dlclose can't
    // free a callback that is still referenced mid-frame. Makes reload deterministic.
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewunload",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->hardClose();
                                                        return "ok\n";
                                                    },
                                                });

    // free-arrange desktop mode toggle:  hyprctl gloviewdesktop
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewdesktop",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->toggleDesktop();
                                                        return "ok\n";
                                                    },
                                                });

    // all-workspaces (expo) view toggle:  hyprctl gloviewall — opens into expo if closed
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewall",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->toggleAllWorkspaces();
                                                        return "ok\n";
                                                    },
                                                });

    // alt-tab cycling:  hyprctl gloviewalttab / gloviewalttabback — closed → opens into expo
    // seeded on the previous window; open → advances the cycle. See gloview:alttab[back].
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewalttab",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->altTabInvoke(false);
                                                        return "ok\n";
                                                    },
                                                });
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewalttabback",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->altTabInvoke(true);
                                                        return "ok\n";
                                                    },
                                                });

    const bool isLua = Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA;
    bool       luaOk = false;
    if (isLua) {
        luaOk = HyprlandAPI::addLuaFunction(handle, "gloview", "toggle", luaToggle);
        HyprlandAPI::addLuaFunction(handle, "gloview", "open", luaOpen);
        HyprlandAPI::addLuaFunction(handle, "gloview", "close", luaClose);
        HyprlandAPI::addLuaFunction(handle, "gloview", "desktop", luaDesktop);
        HyprlandAPI::addLuaFunction(handle, "gloview", "allworkspaces", luaAllWorkspaces);
        HyprlandAPI::addLuaFunction(handle, "gloview", "alttab", luaAltTab);
        HyprlandAPI::addLuaFunction(handle, "gloview", "alttabback", luaAltTabBack);
    }
    (void)luaOk;

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
