#include "overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <utility>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

// shouldRenderWindow trampoline: file-static (anonymous-namespace) so its
// address is stable across this TU and g_shouldRenderOrig stays private to
// initialize()/dtor.
namespace {
using PSHOULDRENDER = bool (*)(void *, PHLWINDOW, PHLMONITOR);
PSHOULDRENDER g_shouldRenderOrig = nullptr;

bool hkShouldRenderWindow(void *thisptr, PHLWINDOW window, PHLMONITOR monitor) {
  if (g_overview && g_overview->shouldHideWindow(window, monitor))
    return false;
  return g_shouldRenderOrig ? g_shouldRenderOrig(thisptr, window, monitor) : true;
}

// damageSurface trampoline. commitWindow() calls
// IHyprRenderer::damageSurface() unconditionally for every visible window on
// buffer commit (Window.cpp) — "visible" means !isHidden() && workspace
// visible, NOT "passes shouldRenderWindow". Windows hidden by the overview are
// NOT isHidden(), so a playing video keeps damaging the monitor on every frame
// → the compositor recomposites the whole overlay at video rate (~30% iGPU).
// Suppress damage for surfaces whose window gloview hides: the overview draws
// frozen FBO snapshots, so the hidden window's commits must not schedule a
// frame. (Client keeps producing buffers; it just stops driving the
// compositor — exactly like a genuinely hidden window.)
using PDAMAGESURFACE = void (*)(void *, SP<CWLSurfaceResource>, double, double,
                                double);
PDAMAGESURFACE g_damageSurfaceOrig = nullptr;

void hkDamageSurface(void *thisptr, SP<CWLSurfaceResource> pSurface, double x,
                     double y, double scale) {
  if (g_overview && g_overview->active() && pSurface) {
    const auto WLSURF = Desktop::View::CWLSurface::fromResource(pSurface);
    if (WLSURF) {
      const auto view = WLSURF->view();
      if (view) {
        const auto w = Desktop::View::CWindow::fromView(view);
        if (w) {
          const auto mon = g_overview->monitor();
          if (g_overview->shouldHideWindow(w, mon)) {
            // The window is hidden by the overview (drawn into a tile, or on
            // the active workspace behind the opaque backdrop). Two cases:
            //
            //   A) Window is a tile preview — renderWindowLive draws its LIVE
            //      surface into the tile. In live mode we want these surfaces'
            //      damage to propagate so video/animations stay live; in
            //      snapshot mode the tile is frozen, so suppress.
            //
            //   B) Window is on the active workspace but NOT a tile — e.g. a
            //      terminal with a blinking cursor, or a transparent window
            //      with blur behind the backdrop. Fully invisible to the user,
            //      so suppress always: these are the dominant cause of the high
            //      GPU load while the overview sits idle (a single blinking-
            //      cursor terminal pins the compositor at ~20-30% iGPU).
            //
            // shouldHideWindow covers both; distinguish by asking whether the
            // window is one of OUR tile windows.
            if (g_overview->snapshotMode() || !g_overview->isTileWindow(w))
              return; // suppress — frozen snapshot tile OR hidden non-tile
            // else: live tile preview → fall through so buffer commits schedule
            // a frame and the preview stays live.
          }
        }
      }
    }
  }
  if (g_damageSurfaceOrig)
    g_damageSurfaceOrig(thisptr, pSurface, x, y, scale);
}
} // namespace

Overview::Overview(HANDLE handle) : m_handle(handle) {}

Overview::~Overview() {
  // stop rendering before state/hooks tear down, so any in-flight frame sees an
  // inactive overview and no dangling refs.
  m_active = false;
  m_opening = false;
  restoreLayers(); // never leave a bar stuck at alpha 0 if we're torn down
                   // mid-hide
  restoreFill();   // never leave a window's surface stuck stretching its small
                   // buffer
  for (const auto &wref :
       m_newWorkspaces) // don't leak any persistent workspace held this session
    if (const auto ws = wref.lock())
      ws->setPersistent(false);
  m_newWorkspaces.clear();
  m_tiles.clear();
  m_strip.clear();
  cancelPendingClick(); // never let a click-timer callback run against a
                        // half-destroyed Overview
  if (m_shouldRenderHook) {
    HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderHook);
    m_shouldRenderHook = nullptr;
  }
  if (m_damageSurfaceHook) {
    HyprlandAPI::removeFunctionHook(m_handle, m_damageSurfaceHook);
    m_damageSurfaceHook = nullptr;
  }
  g_shouldRenderOrig = nullptr;
  g_damageSurfaceOrig = nullptr;
}

bool Overview::initialize() {
  auto &events = Event::bus()->m_events;

  m_renderStageL = events.render.stage.listen(
      [this](eRenderStage stage) { renderStage(stage); });
  m_mouseButtonL = events.input.mouse.button.listen(
      [this](const IPointer::SButtonEvent &event, Event::SCallbackInfo &info) {
        const auto copied = event;
        if (onMouseButton(copied))
          info.cancelled = true;
      });
  m_mouseAxisL = events.input.mouse.axis.listen(
      [this](const IPointer::SAxisEvent &event, Event::SCallbackInfo &info) {
        if (onMouseAxis(event))
          info.cancelled = true;
      });
  m_mouseMoveL = events.input.mouse.move.listen(
      [this](const Vector2D &, Event::SCallbackInfo &) { onMouseMove(); });
  m_keyL = events.input.keyboard.key.listen(
      [this](const IKeyboard::SKeyEvent &event, Event::SCallbackInfo &info) {
        bool cancel = false;
        onKey(event, cancel);
        if (cancel)
          info.cancelled = true;
      });

  // Own clean focus-history tracker for Alt-Tab's MRU ordering — see
  // m_focusHistory's comment in overview.hpp for why we don't just read
  // Desktop::History::windowTracker() directly (it gets polluted by our own
  // syncFocus() calls). Seed it once, right now, from Hyprland's own
  // tracker — at plugin load nothing has run yet, so it's still trustworthy,
  // and this avoids Alt-Tab falling back to "linear" ordering for a while
  // after every reload just because nothing had happened yet to populate our
  // own vector from scratch.
  for (const auto &wref : Desktop::History::windowTracker()->fullHistory())
    if (const auto w = wref.lock())
      m_focusHistory.emplace_back(w);
  m_windowActiveL = events.window.active.listen(
      [this](PHLWINDOW w, Desktop::eFocusReason) {
        if (m_suppressHistoryTrack || !w)
          return;
        const PHLWINDOWREF ref = w;
        std::erase(m_focusHistory, ref);
        m_focusHistory.emplace_back(ref);
        if (m_focusHistory.size() > 128) // periodic prune, mirrors
                                         // CWindowHistoryTracker::gc()
          std::erase_if(m_focusHistory,
                        [](const auto &r) { return r.expired(); });
      });

  const auto matches =
      HyprlandAPI::findFunctionsByName(m_handle, "shouldRenderWindow");
  void *addr = nullptr;
  for (const auto &mt : matches) {
    // CMonitor moved into namespace Monitor:: in Hyprland 0.56 — the mangled
    // symbol's demangled form reflects the real (non-aliased) type, not the
    // PHLMONITOR alias, so the match string has to track it.
    if (mt.demangled.find("shouldRenderWindow(Hyprutils::Memory::"
                          "CSharedPointer<Desktop::View::CWindow>, "
                          "Hyprutils::Memory::CSharedPointer<Monitor::"
                          "CMonitor>)") != std::string::npos) {
      addr = mt.address;
      break;
    }
  }
  if (!addr) {
    HyprlandAPI::addNotification(
        m_handle, "[gloview] could not find shouldRenderWindow to hook",
        CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
    return false;
  }
  m_shouldRenderHook = HyprlandAPI::createFunctionHook(
      m_handle, addr, reinterpret_cast<void *>(&hkShouldRenderWindow));
  if (!m_shouldRenderHook || !m_shouldRenderHook->hook()) {
    // Most common cause: ANOTHER gloview instance already holds this hook (e.g.
    // the hyprpm-installed copy autoloaded at session start while the dev
    // `reload` target loads the build path) — Hyprland can't trampoline an
    // already-hooked prologue.
    HyprlandAPI::addNotification(
        m_handle,
        "[gloview] failed to hook shouldRenderWindow — is another gloview "
        "instance loaded (hyprpm)?",
        CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
    return false;
  }
  g_shouldRenderOrig =
      reinterpret_cast<PSHOULDRENDER>(m_shouldRenderHook->m_original);

  // Hook IHyprRenderer::damageSurface to stop hidden windows (a playing video
  // under the overview) from damaging the monitor every frame — see
  // hkDamageSurface's comment. commitWindow → damageSurface is the ONLY live
  // path left driving the overview's recomposite at video rate once the
  // snapshots are frozen; without this the overview sits at ~30% iGPU with a
  // video tile even though it draws static FBO copies.
  const auto matchesDamage =
      HyprlandAPI::findFunctionsByName(m_handle, "damageSurface");
  void *addrDamage = nullptr;
  for (const auto &mt : matchesDamage) {
    if (mt.demangled.find("damageSurface(Hyprutils::Memory::"
                          "CSharedPointer<CWLSurfaceResource>, double, double, "
                          "double)") != std::string::npos) {
      addrDamage = mt.address;
      break;
    }
  }
  if (addrDamage) {
    m_damageSurfaceHook = HyprlandAPI::createFunctionHook(
        m_handle, addrDamage, reinterpret_cast<void *>(&hkDamageSurface));
    if (m_damageSurfaceHook && m_damageSurfaceHook->hook())
      g_damageSurfaceOrig =
          reinterpret_cast<PDAMAGESURFACE>(m_damageSurfaceHook->m_original);
    else {
      HyprlandAPI::addNotification(
          m_handle, "[gloview] failed to hook damageSurface",
          CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
      m_damageSurfaceHook = nullptr;
    }
  } else {
    HyprlandAPI::addNotification(
        m_handle, "[gloview] could not find damageSurface to hook",
        CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
  }
  return true;
}

// ---- config -----------------------------------------------------------------

// Read through the V2 value() accessor (ConfigRegistry in overview.hpp): the
// deprecated getConfigValue() ignored Lua-config values, so they had no effect.
int Overview::cfgInt(const char *name, int fallback) const {
  const auto it = g_config.ints.find(name);
  return (it != g_config.ints.end() && it->second)
             ? static_cast<int>(it->second->value())
             : fallback;
}

float Overview::cfgFloat(const char *name, float fallback) const {
  const auto it = g_config.floats.find(name);
  return (it != g_config.floats.end() && it->second)
             ? static_cast<float>(it->second->value())
             : fallback;
}

std::string Overview::cfgStr(const char *name, const char *fallback) const {
  const auto it = g_config.strings.find(name);
  return (it != g_config.strings.end() && it->second) ? it->second->value()
                                                       : std::string{fallback};
}

std::string Overview::cursorMode() const {
    return cfgStr("plugin:gloview:cursor_mode", "auto");
}

namespace {
// Accepts gloview's long-established "0xAARRGGBB" convention, plus the
// shorter "0xRRGGBB" (alpha assumed FF), "#AARRGGBB"/"#RRGGBB", and bare 6/8
// hex digits with no prefix at all — whatever's easiest to paste out of a
// palette generator's own output or an existing hyprland.conf. Single byte
// order throughout (AARRGGBB, matching argb()/the rest of this codebase) —
// deliberately NOT Hyprland's own rgba(RRGGBBAA)/rgb(RRGGBB) forms, since
// those use the OPPOSITE alpha position and would silently produce a wrong
// color if guessed at; since these fields are no longer Hyprlang's native
// COLOR type (see the ConfigRegistry comment in overview.hpp for why), that
// parsing was never inherited for free and re-implementing its exact
// ambiguity isn't worth it when gloview's own docs only ever taught 0xAARRGGBB
// anyway. Returns `fallback` (parsed the same way) on anything unparseable.
Hyprlang::INT parseHexColor(std::string s, Hyprlang::INT fallback) {
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
  uint64_t v = 0;
  try {
    v = std::stoull(s, nullptr, 16);
  } catch (...) { return fallback; }
  if (s.size() == 6) // RRGGBB, no alpha given — fully opaque
    v |= 0xFF000000ULL;
  return static_cast<Hyprlang::INT>(v);
}
} // namespace

// Live Hyprland gradient for a whitelisted scheme-role keyword — see the
// exhaustive safety rationale on the declaration in overview.hpp (short
// version: an unknown Config::mgr()->getConfigValue() path SIGABRTs the whole
// compositor, so this can NEVER bind a runtime/user-supplied path, only these
// literals). Each verified present in ConfigValues.cpp on the pinned/running
// source before being added here:
//   general:col.active_border / inactive_border   (primary / secondary)
//   group:col.border_locked_active                (error / danger)
//   group:col.border_active / border_inactive     (group_active / group_inactive)
// static locals per branch: bind once, like Hyprland's own internal
// CConfigValue users — the bound pointer stays valid and current across
// config reloads, only the ONE-TIME lookup is cached.
const Config::CGradientValueData *Overview::schemeGradient(const std::string &role) const {
  if (role == "primary" || role == "accent") {
    static auto V = CConfigValue<Config::IComplexConfigValue>("general:col.active_border");
    return static_cast<const Config::CGradientValueData *>(V.ptr());
  }
  if (role == "secondary") {
    static auto V = CConfigValue<Config::IComplexConfigValue>("general:col.inactive_border");
    return static_cast<const Config::CGradientValueData *>(V.ptr());
  }
  if (role == "error" || role == "danger") {
    static auto V = CConfigValue<Config::IComplexConfigValue>("group:col.border_locked_active");
    return static_cast<const Config::CGradientValueData *>(V.ptr());
  }
  if (role == "group_active") {
    static auto V = CConfigValue<Config::IComplexConfigValue>("group:col.border_active");
    return static_cast<const Config::CGradientValueData *>(V.ptr());
  }
  if (role == "group_inactive") {
    static auto V = CConfigValue<Config::IComplexConfigValue>("group:col.border_inactive");
    return static_cast<const Config::CGradientValueData *>(V.ptr());
  }
  return nullptr;
}

// ONE field does everything a color option needs: a manual hex literal, or a
// scheme-role keyword (optionally "role:AA" for a custom alpha) — see the
// declaration in overview.hpp for the full grammar. `fallback` is this
// option's own default written the exact same way a user would.
Hyprlang::INT Overview::cfgColorScheme(const char *base, const char *fallback) const {
  const std::string key = std::string("plugin:gloview:") + base;
  std::string        raw = cfgStr(key.c_str(), fallback);
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())))
    raw.erase(raw.begin());
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())))
    raw.pop_back();
  if (raw.empty())
    return parseHexColor(fallback, 0xffffffffLL);

  // Optional "<role>:AA" alpha override — exactly 2 trailing hex digits after
  // the LAST colon. Never ambiguous with a manual hex value: none of the
  // supported hex forms (0x../#../bare) ever contain a colon, so seeing one
  // at all already means this is scheme-role syntax, not a color literal.
  std::string rolePart      = raw;
  int         alphaOverride = -1;
  if (const auto pos = raw.rfind(':'); pos != std::string::npos && raw.size() - pos == 3) {
    const std::string tail = raw.substr(pos + 1);
    if (std::isxdigit(static_cast<unsigned char>(tail[0])) && std::isxdigit(static_cast<unsigned char>(tail[1]))) {
      rolePart      = raw.substr(0, pos);
      alphaOverride = static_cast<int>(std::stoul(tail, nullptr, 16));
    }
  }
  std::string roleLower = rolePart;
  for (auto &c : roleLower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  if (const auto *grad = schemeGradient(roleLower); grad && !grad->m_colors.empty()) {
    const auto &c              = grad->m_colors.front(); // 0..1 doubles
    const auto  chan           = [](double v) { return static_cast<Hyprlang::INT>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
    const Hyprlang::INT defA   = (parseHexColor(fallback, 0xffffffffLL) >> 24) & 0xFF; // this option's own default alpha
    const Hyprlang::INT a      = alphaOverride >= 0 ? alphaOverride : defA;
    return (a << 24) | (chan(c.r) << 16) | (chan(c.g) << 8) | chan(c.b);
  }
  // Not a recognized role (or its live gradient isn't readable yet) — treat
  // the whole string as a manual color literal instead of silently going
  // black; parseHexColor() rejects anything that isn't 6/8 hex digits (a typo
  // like "primry" falls straight through to the option's own default here).
  return parseHexColor(raw, parseHexColor(fallback, 0xffffffffLL));
}

Overview::Anchor Overview::stripAnchor() const {
  std::string a = cfgStr("plugin:gloview:anchor", "");
  if (a.empty()) // back-compat: the old top|bottom knob
    a = cfgStr("plugin:gloview:bar_position", "top");
  if (a == "bottom")
    return Anchor::Bottom;
  if (a == "left")
    return Anchor::Left;
  if (a == "right")
    return Anchor::Right;
  return Anchor::Top;
}

bool Overview::stripHorizontal() const {
  const Anchor a = stripAnchor();
  return a == Anchor::Top || a == Anchor::Bottom;
}

double Overview::stripThickness() const {
  const auto m = m_monitor.lock();
  if (!m)
    return 150.0;
  // clamp against the axis perpendicular to the band: height for a horizontal
  // strip, width for a vertical one.
  const double cross = stripHorizontal() ? m->m_size.y : m->m_size.x;
  return std::clamp(
      static_cast<double>(cfgInt("plugin:gloview:strip_height", 150)), 100.0,
      cross * 0.42);
}

double Overview::stripOffset() const {
  const auto m = m_monitor.lock();
  if (!m)
    return 0.0;
  // distance from the anchored edge. 0 = flush (no auto bar gap); purely
  // cosmetic.
  const double cross = stripHorizontal() ? m->m_size.y : m->m_size.x;
  return std::clamp(
      static_cast<double>(cfgInt("plugin:gloview:strip_offset", 0)), 0.0,
      cross * 0.4);
}

LRect Overview::stripBand() const {
  const auto m = m_monitor.lock();
  if (!m)
    return LRect{0, 0, 0, 0};
  const double T = stripThickness();
  const double off = stripOffset();
  const double W = m->m_size.x;
  const double H = m->m_size.y;
  switch (stripAnchor()) {
  case Anchor::Bottom:
    return LRect{0, H - T - off, W, T};
  case Anchor::Left:
    return LRect{off, 0, T, H};
  case Anchor::Right:
    return LRect{W - T - off, 0, T, H};
  case Anchor::Top:
  default:
    return LRect{0, off, W, T};
  }
}

Vector2D Overview::stripSlide(double e) const {
  // slide the strip in from its own edge as it fades (distance ∝ band
  // thickness).
  const double d = (1.0 - e) * stripThickness() * 0.55;
  switch (stripAnchor()) {
  case Anchor::Bottom:
    return Vector2D{0.0, d};
  case Anchor::Left:
    return Vector2D{-d, 0.0};
  case Anchor::Right:
    return Vector2D{d, 0.0};
  case Anchor::Top:
  default:
    return Vector2D{0.0, -d};
  }
}

bool Overview::blurEnabled() const { return blurStrength() > 0.0F; }

// plugin:gloview:blur is a float: 0 = off, 1 = full, between scales blur
// strength via the pass alpha. Clamped 0..1.
float Overview::blurStrength() const {
  return std::clamp(cfgFloat("plugin:gloview:blur", 1.0F), 0.0F, 1.0F);
}

int Overview::blurPasses() const {
  return std::clamp(cfgInt("plugin:gloview:blur_passes", 3), 1, 16);
}

int Overview::blurSize() const {
  return std::clamp(cfgInt("plugin:gloview:blur_size", 8), 1, 200);
}

int Overview::blurResolution() const {
  return std::clamp(cfgInt("plugin:gloview:blur_resolution", 4), 1, 32);
}

// ---- open / close -----------------------------------------------------------

void Overview::toggle() {
  if (m_active && m_opening)
    close();
  else
    open(); // opens into the tidy grid; Shift / gloview:desktop flips to the
            // canvas
}

// gloview:allworkspaces — toggle the "expo" view (every window on the monitor).
// OPENS the overview if closed (so it works as a single-key bind); while open
// flips expo on/off and glides tiles. m_allOverride overrides
// show_all_workspaces until close (deactivate → -1).
void Overview::toggleAllWorkspaces() {
  if (!(m_active && m_opening)) {
    m_allOverride =
        1; // open directly into expo (set BEFORE open() so buildTiles sees it)
    open();
    if (!m_active) // open declined (no monitor / nothing to show) — don't leave
                   // it armed
      m_allOverride = -1;
    return;
  }
  m_allOverride = showAllWorkspaces() ? 0 : 1;
  // rebuild from the new membership and glide tiles into place (chrome settled
  // at progress 1).
  std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
  oldBoxes.reserve(m_tiles.size());
  for (size_t i = 0; i < m_tiles.size(); ++i)
    if (const auto win = m_tiles[i].win.lock())
      oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));
  replayReflow(oldBoxes);
}

// gloview:alttab / gloview:alttabback — bind these to the SAME modifier+key
// you'd otherwise bind gloview:allworkspaces to (e.g. "SUPER, TAB,
// gloview:alttab"). Closed → opens straight into the all-workspaces view with
// the tiles reordered into MRU (most-recently-used) order instead of their
// usual spatial layout, and the cursor already on the PREVIOUSLY focused window
// (rank 0 after buildAltTabRank()'s rotation — see its comment) — the very act
// of opening already counts as "one tab", exactly like a real alt-tab's first
// press, and this initial landing spot is ALWAYS the previously focused window
// regardless of whether it was gloview:alttab or gloview:alttabback that opened
// it; only SUBSEQUENT taps step by direction. Already open → advances the cycle
// one step (repeated physical taps of the bound key while the modifier stays
// held — Hyprland re-invokes the dispatcher on every press, exactly like
// holding Alt and tapping Tab). Whether releasing the modifier commits (focuses
// the selection & closes, like a normal alt-tab) is separate — see
// alt_tab_commit_on_release and the modifier-release handling in onKey.
void Overview::altTabInvoke(bool reverse) {
  if (!(m_active && m_opening)) {
    // Snapshot the MRU rank BEFORE anything else — including open()'s own
    // focus-seeking — can perturb Hyprland's real focus history, and prime
    // m_altTabbing so buildTiles() (called from inside open()) sorts the fresh
    // tiles by it instead of starting plain.
    m_altTabbing = true;
    m_altTabRank = buildAltTabRank();
    m_allOverride = 1; // open directly into expo, same as gloview:allworkspaces
    open(/*viaAltTab=*/true);
    if (!m_active) {
      m_allOverride = -1;
      m_altTabbing = false;
      m_altTabRank.clear();
      return;
    }
    // buildAltTabRank() rotates the raw ranking so rank 0 is the PREVIOUSLY
    // focused window, not the current one — m_tiles is sorted by that rank
    // (buildTiles()), so index 0 is already both the right tile AND the right
    // grid slot (layoutTiles() preserves the order). Land there directly,
    // regardless of `reverse`, same as a real alt-tab's first press. "linear"
    // mode has no rank map (m_altTabRank stays empty, buildTiles() left the
    // natural build order alone), so there's no rotated "previous" slot to rely
    // on — keep the old fallback of stepping straight to index 1.
    m_selected = (m_altTabRank.empty() && m_tiles.size() > 1) ? 1 : 0;
    syncFocus();
    damage();
    return;
  }
  stepAltTab(reverse ? -1 : 1);
}

void Overview::toggleDesktop() {
  if (m_active && m_opening)
    setDesktopMode(!m_desktopMode);
}

// Flip grid <-> canvas while open, gliding previews into the new layout. Canvas
// is purely visual — NO real window is floated/moved/resized.
void Overview::setDesktopMode(bool on) {
  m_desktopMode = on;
  m_canvasPos
      .clear(); // each entry into the canvas starts from the real positions

  std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
  oldBoxes.reserve(m_tiles.size());
  for (size_t i = 0; i < m_tiles.size(); ++i)
    if (const auto win = m_tiles[i].win.lock())
      oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));
  m_hovered = m_hoveredStrip = -1;
  m_selected = -1;
  // Canvas is PURELY VISUAL — never floats/moves/resizes a real window.
  // Survivors don't shuffle on add/remove: syncTiles parks them in m_canvasPos
  // (frozen slot) + frozen aspect, so only the newcomer flows (residual: a
  // re-tiled survivor's live content is over-covered to fill its slot).
  replayReflow(
      oldBoxes); // rebuild + glide previews grid<->canvas, chrome pinned at 1
}

void Overview::open(bool viaAltTab) {
  if (m_active && m_opening)
    return;

  const auto m = State::monitorState()
                    ->query()
                    .vec(g_pInputManager->getMouseCoordsInternal())
                    .run();
  if (!m || !m->m_activeWorkspace)
    return;

  m_monitor = m;
  m_workspace = m->m_activeWorkspace;
  m_liveWsAtOpen =
      m->m_activeWorkspace; // exit_on_switch watches this for external changes
  m_hovered = m_hoveredStrip = -1;

  // Force-rebuild the pre-blurred bg cache (m_blurFB): built once at boot from
  // a stale pre-window scene and only re-dirtied on bg damage, so the first
  // overview's band else samples the cold cache and renders see-through
  // ("transparent strip").
  m->m_blurFBDirty = true;

  // Clear drag/press state: a mid-drag dismiss (ESC/TAB/click) skips the
  // release handler, so the next open would inherit m_dragging + stale
  // m_pressTile (a tile floats at cursor).
  m_pressTile = -1;
  m_dragging = false;
  m_dragX = m_dragY = m_pressX = m_pressY = m_grabDX = m_grabDY = 0.0;
  m_pressButton = 0;
  m_pressStripItem = -1;
  m_pressStripWin = -1;
  m_dragStripWin.reset();
  m_pendingFocus.reset(); // no stale carry-over from a previous session
  cancelPendingClick();   // ditto for any close_trigger=doubleclick timer
  // altTabInvoke primes m_altTabbing + m_altTabRank BEFORE calling us
  // (viaAltTab=true) so buildTiles() below sorts the fresh tiles by that
  // snapshot — a normal open starts clean.
  if (!viaAltTab) {
    m_altTabbing = false;
    m_altTabRank.clear();
  }
  m_canvasPos.clear();
  m_desktopMode = false; // open into the tidy grid; Shift / gloview:desktop
                         // flips to the canvas
  m_newCardAnim = false;
  m_newCardId = 0;
  for (const auto &wref :
       m_newWorkspaces) // defensive: shouldn't still hold anything here, but
                        // don't leak if it does
    if (const auto ws = wref.lock())
      ws->setPersistent(false);
  m_newWorkspaces.clear();

  buildTiles();
  buildStrip();
  if (m_tiles.empty() && m_strip.size() <= 1) // nothing to show
    return;

  layoutTiles();

  // seed keyboard selection on the focused window (else first tile) for
  // arrow-nav/Enter. For an alt-tab open this also lands on rank 0 (m_tiles was
  // just MRU-sorted by buildTiles(), and the focused window IS rank 0), which
  // altTabInvoke then steps once more onto rank 1 (the previous window).
  m_selected = m_tiles.empty() ? -1 : 0;
  if (const auto fw = Desktop::focusState()->window()) {
    for (size_t i = 0; i < m_tiles.size(); ++i)
      if (m_tiles[i].win.lock() == fw) {
        m_selected = static_cast<int>(i);
        break;
      }
  }

  m_active = true;
  m_opening = true;
  m_reflowing = false;
  m_pendingDeactivate = false;
  m_progress = 0.0;
  m_animStart = std::chrono::steady_clock::now();
  hideLayers(); // fade bars out (no-op unless hide_top/overlay_layers set)
  m_cursor.onOpen(m, cursorMode());
  m_backdropDrawn = false; // draw fresh wallpaper into backdrop source FBO
  m_blur.drop();           // re-blur from scratch on the next rendered frame
  damage();
}

void Overview::close() {
  if (!m_active)
    return;
  m_opening = false;
  m_reflowing = false;

  // Once ANYTHING starts a close (this one included — the doubleclick timer's
  // own callback routes back through here), a still-pending single-click
  // timer is stale: the tile it targeted is about to stop existing in the
  // overview one way or another, so let it fire later would either be
  // redundant or act on gone state.
  cancelPendingClick();

  // Re-seed every tile's `natural` to the window's REAL settled geometry so the
  // close anim glides target -> real pixel-perfect (renderMainWindows assumes
  // "progress 0 == real"). `natural` gets repurposed as a reflow START box
  // (always in desktop mode, after any swap/drop reflow), so without this
  // windows jump on close.
  if (const auto m = m_monitor.lock()) {
    for (auto &t : m_tiles) {
      if (const auto w = t.win.lock()) {
        const auto p = w->positionAnimation()->goal();
        const auto s = w->sizeAnimation()->goal();
        t.natural = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                          std::max(1.0, s.x), std::max(1.0, s.y)};
      }
    }
  }

  restoreLayers(); // bars fade back in over the close animation, not in a pop
                   // at the end
  m_cursor.onClose();
  m_animStart = std::chrono::steady_clock::now() -
                std::chrono::milliseconds(static_cast<long>(
                    (1.0 - m_progress) *
                    std::max(1, cfgInt("plugin:gloview:duration", 360))));
  damage();
}

// Immediate, animation-free teardown for the UNLOAD path (`hyprctl
// gloviewunload`, run before unloading). close() only *starts* the close anim;
// unloading the .so before it finishes can yank the library while an overlay
// COverlayPass element or a pending recapture timer — both holding callbacks in
// this .so — is still referenced → SEGV / IPC-dead spin. hardClose drops all
// that state synchronously + damages, so the next frame is plugin-free ("flush
// frame") and dlclose is safe. Hooks stay installed (harmless at
// m_active=false; dtor removes them). Unlike deactivate it does NOT commit the
// displayed workspace — unload snaps back to the live one.
void Overview::hardClose() {
  // Same hazard the old recapture-timer cleanup here used to guard against
  // (a fire lambda capturing `this` in this .so, still pending at unload):
  // the click timer is the only one of those left now.
  cancelPendingClick();

  restoreLayers(); // never leave a bar stuck at alpha 0 if we tear down
                   // mid-hide
  restoreFill();   // never leave a window's surface stuck stretching its small
                   // buffer
  for (const auto &wref :
       m_newWorkspaces) // don't leak any held-persistent "+"-created workspace
    if (const auto ws = wref.lock())
      ws->setPersistent(false);
  m_newWorkspaces.clear();

  m_active = false;
  m_opening = false;
  m_reflowing = false;
  m_pendingDeactivate = false;
  m_desktopMode = false;
  m_newCardAnim = false;
  m_newCardId = 0;
  m_progress = 0.0;
  m_altTabbing = false;
  m_altTabRank.clear();
  m_pressStripItem = -1;
  m_pressStripWin = -1;
  m_dragStripWin.reset();
  m_tiles.clear();
  m_strip.clear();
  m_snapshots.clear();
  m_canvasPos.clear();
  m_hovered = m_hoveredStrip = -1;
  m_selected = -1;

  damage(); // schedule the plugin-free flush frame
}

// ---- collection -------------------------------------------------------------

// Effective expo state. Runtime m_allOverride wins over the show_all_workspaces
// config; -1 means "follow config".
bool Overview::showAllWorkspaces() const {
  return m_allOverride < 0
             ? (cfgInt("plugin:gloview:show_all_workspaces", 0) != 0)
             : (m_allOverride != 0);
}

// Shared membership predicate for main-area tiles. buildTiles (full rebuild)
// and syncTiles (per-frame add/remove detector) MUST agree, else syncTiles sees
// a phantom diff every frame and reflow-churns.
bool Overview::tileBelongs(const PHLWINDOW &w, const PHLMONITOR &m,
                           const PHLWORKSPACE &ws) const {
  if (!w || !w->m_isMapped || w->isHidden())
    return false;
  const auto wws = w->m_workspace;
  if (!wws)
    return false;
  if (showAllWorkspaces()) { // expo: every window living on this monitor, any
                             // workspace
    if (wws->m_isSpecialWorkspace &&
        cfgInt("plugin:gloview:show_special", 0) == 0)
      return false;
    return wws->m_monitor.lock() == m;
  }
  return wws == ws; // single displayed workspace (may be inactive; fine)
}

bool Overview::shouldHideWindow(const PHLWINDOW &w,
                                const PHLMONITOR &mon) const {
  const auto m = m_monitor.lock();
  if (!m_active || !w || mon != m)
    return false;
  // Fullscreen windows are hidden wholesale while the overview is up. Without
  // this, a workspace-transition force-render of an off-display fullscreen
  // window (Hyprland's renderWorkspaceWindowsFullscreen "pass sucks" loop
  // force-renders fullscreen windows whose workspace is animating/forced)
  // leaks it into currentFB, and the once-cached backdrop blur then keeps
  // showing it even after the transition settles — "a fullscreen terminal
  // instead of the wallpaper". The fullscreen_background feature does NOT
  // re-enable rendering here: the featured window (a fullscreen mpv on the
  // displayed workspace) is sourced directly from its live surface texture by
  // backdropSource(), so no fullscreen window ever needs to draw into the
  // scene.
  if (Fullscreen::controller()->isFullscreen(w))
    return true;
  // hide the windows we draw previews for
  for (const auto &t : m_tiles)
    if (t.win.lock() == w)
      return true;
  // also hide the monitor's active workspace, so displaying a different desktop
  // doesn't bleed the current one through the translucent backdrop.
  if (const auto aw = m->m_activeWorkspace; aw && w->m_workspace == aw)
    return true;
  return false;
}

bool Overview::isTileWindow(const PHLWINDOW &w) const {
    if (!w)
        return false;
    for (const auto &t : m_tiles)
        if (t.win.lock() == w)
            return true;
    return false;
}

void Overview::deactivate() {
  restoreLayers(); // safety net: normally close() already restored; harmless if
                   // empty
  restoreFill(); // drop the fill-small override so real windows render normally
                 // again
  m_canvasPos.clear();

  if (const auto m = m_monitor.lock()) {
    if (const auto ws = m_workspace.lock(); ws && ws != m->m_activeWorkspace) {
      const auto oldWs = m->m_activeWorkspace; // capture BEFORE the switch —
                                               // this is what animates OUT
      m->changeWorkspace(ws, false, true, false);
      // changeWorkspace() ALWAYS starts Hyprland's own native workspace-switch
      // slide — verified against the pinned source (Monitor.cpp): its calls to
      // CDesktopAnimationManager::startAnimation() for both the outgoing and
      // incoming workspace hardcode instant=false, and that parameter isn't
      // exposed through changeWorkspace()'s own signature, so there's no way
      // to ask it not to. We still want everything ELSE this call does (IPC
      // workspace events, pinned-window handling, mouse-move sim, layout
      // recalculate, focus fallback) — only the visible slide is unwanted
      // here: gloview's own close glide (m_progress) JUST finished playing
      // its own "arriving at this window" motion, so Hyprland's native one
      // starting immediately after reads as a jarring extra hop tacked onto
      // the end, not a continuation of it.
      //
      // Fix: let it start normally, then instantly finish it. startAnimation()
      // animates exactly two properties per workspace — m_alpha and
      // m_renderOffset (DesktopAnimationManager.cpp) — by assigning each a
      // GOAL and letting them interpolate there over their configured
      // duration. Warping straight to that already-assigned goal
      // (setValueAndWarp keeps the goal, skips the interpolation) finishes
      // the animation in this same frame instead of over its duration — the
      // switch still visibly happens (nothing here skips it, unlike
      // internal=true, which would also silently drop the IPC events and
      // recalc), it just doesn't visibly slide.
      if (oldWs && oldWs != ws) {
        oldWs->m_alpha->setValueAndWarp(oldWs->m_alpha->goal());
        oldWs->m_renderOffset->setValueAndWarp(oldWs->m_renderOffset->goal());
      }
      ws->m_alpha->setValueAndWarp(ws->m_alpha->goal());
      ws->m_renderOffset->setValueAndWarp(ws->m_renderOffset->goal());
    }
  }

  // Re-assert the explicitly committed focus (see m_pendingFocus's comment in
  // overview.hpp) as the very last real-desktop action of this session.
  // For a cross-workspace commit, focusAndClose() deliberately skipped its own
  // fullWindowFocus() call (see its comment) so this changeWorkspace() above
  // is the FIRST time the real desktop actually switches — keeping Hyprland's
  // native workspace-switch animation from playing until our own close glide
  // is fully done. That changeWorkspace() call's own auto-focus resolves via
  // CWorkspace::getLastFocusedWindow(), which can still land on whatever was
  // actually focused there before this session rather than the window we just
  // committed to — re-focusing it one more time right here guarantees the
  // committed choice wins over stale history. For a same-workspace commit,
  // focusAndClose() already focused it immediately; this is just a harmless
  // backstop that re-confirms the same thing.
  if (const auto pw = m_pendingFocus.lock(); pw && pw->m_isMapped && !pw->isHidden())
    Desktop::focusState()->fullWindowFocus(pw, Desktop::FOCUS_REASON_KEYBIND);
  m_pendingFocus.reset();

  // Drop the hold on every "+"/number-key-created workspace this session: each
  // stays if active or a window landed there, otherwise reaps like any empty
  // one. (Used to be a single slot that only released the LAST one created —
  // creating several in one session silently leaked the rest as permanent
  // phantom persistent workspaces — task/bug #4.)
  for (const auto &wref : m_newWorkspaces)
    if (const auto ws = wref.lock())
      ws->setPersistent(false);
  m_newWorkspaces.clear();

  m_active = false;
  m_opening = false;
  m_reflowing = false;
  m_pendingDeactivate = false;
  m_desktopMode = false;
  m_allOverride =
      -1; // next open follows plugin:gloview:show_all_workspaces again
  m_altTabbing = false;
  m_altTabRank.clear();
  m_newCardAnim = false;
  m_newCardId = 0;
  m_progress = 0.0;
  m_tiles.clear();
  m_strip.clear();
  m_snapshots.clear();
  m_hovered = m_hoveredStrip = -1;
  m_selected = -1;
  damage();
}

void Overview::damage() const {
  if (const auto m = m_monitor.lock(); m && g_pHyprRenderer)
    g_pHyprRenderer->damageMonitor(m);
}

// snapshot preview mode (plugin:gloview:preview_mode == "snapshot"): grab each
// tile/strip window's main surface texture into m_snapshots so
// renderWindowLive() can draw it statically. The window's LAST COMMITTED
// texture (m_current.texture) — windows are hidden while the overview is up,
// so this is the freshest frame they'll ever hand us; it stays valid as long
// as we hold the SP. Only ADDS missing entries (keys already captured stay
// frozen — that's the point of snapshot mode), so a window that appears
// mid-session gets one grab and then holds that exact frame; the map is
// cleared wholesale on teardown (deactivate/hardClose).
bool Overview::snapshotMode() const {
  return cfgStr("plugin:gloview:preview_mode", "live") == "snapshot";
}

void Overview::updateSnapshots() {
  if (!snapshotMode())
    return;
  const auto grab = [this](const PHLWINDOW &w) {
    if (!w || !w->wlSurface() || !w->wlSurface()->resource())
      return;
    if (m_snapshots.contains(w.get()))
      return; // already captured → keep it static
    const auto tex = w->wlSurface()->resource()->m_current.texture;
    if (!tex || !tex->ok())
      return;
    // Real pixel copy: the live texture (m_current.texture) is re-uploaded on
    // every client commit (SHM → glTexSubImage2D into the same GL object) or
    // re-imported from a recycled buffer (DMA-BUF → black preview), so holding
    // the SP is NOT a freeze. Render it 1:1 into a private FB instead — the FB
    // texture stays as-is no matter what the client does afterwards.
    const int W = tex->m_size.x;
    const int H = tex->m_size.y;
    if (W <= 0 || H <= 0)
      return;

    auto fb = g_pHyprRenderer->createFB("gloview snapshot");
    if (!fb)
      return;
    fb->alloc(W, H);

    auto       guard = g_pHyprRenderer->bindTempFB(fb);
    const auto oldProjType     = g_pHyprRenderer->m_renderData.projectionType;
    const auto oldFbSize       = g_pHyprRenderer->m_renderData.fbSize;
    const auto oldTransformDmg = g_pHyprRenderer->m_renderData.transformDamage;
    const auto oldModif        = g_pHyprRenderer->m_renderData.renderModif;

    g_pHyprRenderer->m_renderData.fbSize = Vector2D{W, H};
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    g_pHyprRenderer->m_renderData.transformDamage = false;
    g_pHyprRenderer->m_renderData.renderModif     = Render::SRenderModifData{};
    g_pHyprOpenGL->setViewport(0, 0, W, H);

    CRegion fullDamage = {0, 0, (double)W, (double)H};
    g_pHyprRenderer->draw(CClearPassElement::SClearData{{0.F, 0.F, 0.F, 0.F}});
    g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = tex, .box = CBox{0, 0, (double)W, (double)H}, .damage = fullDamage},
                          fullDamage);

    g_pHyprRenderer->m_renderData.fbSize          = oldFbSize;
    g_pHyprRenderer->m_renderData.transformDamage = oldTransformDmg;
    g_pHyprRenderer->m_renderData.renderModif     = oldModif;
    g_pHyprRenderer->setProjectionType(oldProjType);
    const auto PMON = g_pHyprRenderer->m_renderData.pMonitor;
    g_pHyprOpenGL->setViewport(0, 0, PMON ? (int)PMON->m_pixelSize.x : W, PMON ? (int)PMON->m_pixelSize.y : H);
    guard.reset();

    m_snapshots[w.get()] = fb->getTexture();
  };
  for (const auto &t : m_tiles)
    if (const auto w = t.win.lock())
      grab(w);
  for (const auto &it : m_strip)
    for (const auto &sw : it.wins)
      if (const auto w = sw.win.lock())
        grab(w);
}

void Overview::dbg(const std::string &msg) const {
  if (cfgInt("plugin:gloview:debug_logs", 0) != 0 && Log::logger)
    Log::logger->log(Log::INFO, "[gloview] {}", msg);
}

} // namespace gloview
