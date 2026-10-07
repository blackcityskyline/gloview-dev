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

#include "debug/log.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "clock/curves.hpp"
#include "anim.hpp"
#include "config/config.hpp"
#include "overview.hpp"

inline HANDLE                             g_handle = nullptr;
inline std::unique_ptr<gloview::Overview> g_overviewOwned;

namespace {

// null name field = that path is skipped. gloviewunload is UNLOAD-safe:
// hardClose; gloviewclose only starts close, unloading there crashes.
struct Action {
    const char* dispatch;
    const char* hyprctl;
    const char* lua;
    void (*invoke)();
};

constexpr std::array<Action, 9> kActions{{
    {"toggle", "gloview", "toggle",
     [] { if (g_overview) g_overview->toggle(); }},
    {"open", nullptr, "open",
     [] { if (g_overview) g_overview->open(); }},
    {"close", "gloviewclose", "close",
     [] { if (g_overview) g_overview->close(); }},
    {"desktop", "gloviewdesktop", "desktop",
     [] { if (g_overview) g_overview->toggleDesktop(); }},
    {"allworkspaces", "gloviewall", "allworkspaces",
     [] { if (g_overview) g_overview->toggleAllWorkspaces(); }},
    {"togglecarousel", "gloviewcarousel", "togglecarousel",
     [] { if (g_overview) g_overview->toggleCarousel(); }},
    {"alttab", "gloviewalttab", "alttab",
     [] { if (g_overview) g_overview->altTabInvoke(false); }},
    {"alttabback", "gloviewalttabback", "alttabback",
     [] { if (g_overview) g_overview->altTabInvoke(true); }},
    {nullptr, "gloviewunload", nullptr,
     [] { if (g_overview) g_overview->hardClose(); }},
}};

template <size_t I>
int luaInvoke(lua_State*) {
    kActions[I].invoke();
    return 0;
}

}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    // build stamp first: grep the log to confirm a hot-reload loaded the new binary
    gloview::debug::dbg("[gloview] PLUGIN_INIT build=" __DATE__ " " __TIME__);

    gloview::cfg::registerAll(handle);
    gloview::curves::registerBuiltins();

    g_overviewOwned = std::make_unique<gloview::Overview>(handle);
    g_overview      = g_overviewOwned.get();
    if (!g_overview->initialize()) {
        HyprlandAPI::addNotification(handle, "[gloview] initialization failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
        g_overview = nullptr;
        g_overviewOwned.reset();
        // throw so Hyprland ejects us; a half-alive instance blocks later loads
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
            HyprlandAPI::addLuaFunction(handle, "gloview", "curve",
                                        &gloview::curves::luaRegister);
            HyprlandAPI::addLuaFunction(handle, "gloview", "animation",
                                        &gloview::anim::luaSetRule);
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
