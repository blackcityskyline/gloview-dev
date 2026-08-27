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

#include "anim/curves.hpp"
#include "config/config.hpp"
#include "overview.hpp"

inline HANDLE                             g_handle = nullptr;
inline std::unique_ptr<gloview::Overview> g_overviewOwned;

namespace {

// --- every plugin:gloview:* config value, grouped by concern ---
// Color options are plain strings holding a hex literal in any accepted form
// ("0xAARRGGBB", "#RRGGBB", …) — or a palette-resolved hex produced Lua-side
// from a theme module (hyprbars pattern), see config/config.cpp.

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

    // The whole option schema lives in config/config.cpp (grouped by domain).
    gloview::cfg::registerAll(handle);
    gloview::curves::registerBuiltins();

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

        if (isLua) {
            ((kActions[Is].lua &&
              HyprlandAPI::addLuaFunction(handle, "gloview", kActions[Is].lua,
                                          &luaInvoke<Is>)),
             ...);
            // hl.plugin.gloview.curve(name, fn|{bezier}): custom curves
            HyprlandAPI::addLuaFunction(handle, "gloview", "curve",
                                        &gloview::curves::luaRegister);
            // hl.plugin.gloview.animation({leaf=..., enabled=..., speed=...,
            //                              ms=..., curve=..., style=...})
            HyprlandAPI::addLuaFunction(handle, "gloview", "animation",
                                        &gloview::cfg::luaSetAnimRule);
        }
    }(std::make_index_sequence<kActions.size()>{});

    HyprlandAPI::reloadConfig();

    return {
        .name        = "GloView",
        .description = "macOS Mission Control-style overview",
        .author      = "Vergil",
        .version     = "0.3.1-test",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_overview = nullptr;
    g_overviewOwned.reset();
}
