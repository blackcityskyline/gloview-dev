#include "overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/Compositor.hpp>
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
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

// shouldRenderWindow trampolines: file-static (anonymous-namespace) so their
// addresses are stable across this TU and g_shouldRender*Orig stay private to
// initialize()/dtor, which is why the hook plumbing lives together here rather
// than split by theme.
namespace {
using PSHOULDRENDER = bool (*)(void *, PHLWINDOW, PHLMONITOR);
using PSHOULDRENDERWINDOW = bool (*)(void *, PHLWINDOW);
PSHOULDRENDER g_shouldRenderOrig = nullptr;
PSHOULDRENDERWINDOW g_shouldRenderWindowOrig = nullptr;

bool hkShouldRenderWindow(void *thisptr, PHLWINDOW window, PHLMONITOR monitor) {
  if (g_overview) {
    // force-render the snapshotting window (may be on an inactive workspace,
    // which Hyprland's original rejects → blank/grey preview).
    if (g_overview->forceRenderWindow(window))
      return true;
    if (g_overview->shouldHideWindow(window, monitor))
      return false;
  }
  return g_shouldRenderOrig ? g_shouldRenderOrig(thisptr, window, monitor)
                            : true;
}

bool hkShouldRenderWindowAny(void *thisptr, PHLWINDOW window) {
  if (g_overview && g_overview->forceRenderWindow(window))
    return true;
  return g_shouldRenderWindowOrig ? g_shouldRenderWindowOrig(thisptr, window)
                                  : true;
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
  if (m_recaptureTimer && g_pEventLoopManager) {
    m_recaptureTimer->cancel();
    g_pEventLoopManager->removeTimer(m_recaptureTimer);
    m_recaptureTimer.reset();
  }
  if (m_shouldRenderHook) {
    HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderHook);
    m_shouldRenderHook = nullptr;
  }
  if (m_shouldRenderWindowHook) {
    HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderWindowHook);
    m_shouldRenderWindowHook = nullptr;
  }
  g_shouldRenderOrig = nullptr;
  g_shouldRenderWindowOrig = nullptr;
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

  const auto matches =
      HyprlandAPI::findFunctionsByName(m_handle, "shouldRenderWindow");
  void *addr = nullptr;
  void *addrAny = nullptr;
  for (const auto &mt : matches) {
    if (mt.demangled.find("shouldRenderWindow(Hyprutils::Memory::"
                          "CSharedPointer<Desktop::View::CWindow>, "
                          "Hyprutils::Memory::CSharedPointer<CMonitor>)") !=
        std::string::npos) {
      addr = mt.address;
    } else if (mt.demangled.find("shouldRenderWindow(Hyprutils::Memory::"
                                 "CSharedPointer<Desktop::View::CWindow>)") !=
               std::string::npos) {
      addrAny = mt.address;
    }
  }
  if (!addr) {
    HyprlandAPI::addNotification(
        m_handle, "[gloview] could not find shouldRenderWindow to hook",
        CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
    return false;
  }
  if (!addrAny) {
    HyprlandAPI::addNotification(
        m_handle, "[gloview] could not find shouldRenderWindow(window) to hook",
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

  m_shouldRenderWindowHook = HyprlandAPI::createFunctionHook(
      m_handle, addrAny, reinterpret_cast<void *>(&hkShouldRenderWindowAny));
  if (!m_shouldRenderWindowHook || !m_shouldRenderWindowHook->hook()) {
    HyprlandAPI::addNotification(
        m_handle, "[gloview] failed to hook shouldRenderWindow(window)",
        CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
    if (m_shouldRenderWindowHook) {
      HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderWindowHook);
      m_shouldRenderWindowHook = nullptr;
    }
    HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderHook);
    m_shouldRenderHook = nullptr;
    g_shouldRenderOrig = nullptr;
    return false;
  }
  g_shouldRenderWindowOrig = reinterpret_cast<PSHOULDRENDERWINDOW>(
      m_shouldRenderWindowHook->m_original);
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

Hyprlang::INT Overview::cfgColor(const char *name,
                                 Hyprlang::INT fallback) const {
  const auto it = g_config.colors.find(name);
  return (it != g_config.colors.end() && it->second)
             ? static_cast<Hyprlang::INT>(it->second->value())
             : fallback;
}

std::string Overview::cfgStr(const char *name, const char *fallback) const {
  const auto it = g_config.strings.find(name);
  return (it != g_config.strings.end() && it->second) ? it->second->value()
                                                      : std::string{fallback};
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

  const auto m = g_pCompositor->getMonitorFromCursor();
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
  damage();
}

void Overview::close() {
  if (!m_active)
    return;
  m_opening = false;
  m_reflowing = false;

  // Re-seed every tile's `natural` to the window's REAL settled geometry so the
  // close anim glides target -> real pixel-perfect (renderMainWindows assumes
  // "progress 0 == real"). `natural` gets repurposed as a reflow START box
  // (always in desktop mode, after any swap/drop reflow), so without this
  // windows jump on close.
  if (const auto m = m_monitor.lock()) {
    for (auto &t : m_tiles) {
      if (const auto w = t.win.lock()) {
        const auto p = w->m_realPosition->goal();
        const auto s = w->m_realSize->goal();
        t.natural = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                          std::max(1.0, s.x), std::max(1.0, s.y)};
      }
    }
  }

  restoreLayers(); // bars fade back in over the close animation, not in a pop
                   // at the end
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
  // Kill the recapture timer FIRST: its fire lambda captures `this` in this
  // .so, so a tick still pending at unload is the IPC-dead-spin hazard.
  m_recaptureLeft = 0;
  if (m_recaptureTimer) {
    m_recaptureTimer->cancel();
    if (g_pEventLoopManager)
      g_pEventLoopManager->removeTimer(m_recaptureTimer);
    m_recaptureTimer.reset();
  }

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

bool Overview::forceRenderWindow(const PHLWINDOW &w) const {
  return m_capturing && w && m_captureWin && m_captureWin.lock() == w;
}

bool Overview::shouldHideWindow(const PHLWINDOW &w,
                                const PHLMONITOR &mon) const {
  const auto m = m_monitor.lock();
  // While snapshotting, let the window render: makeSnapshot's own
  // shouldRenderWindow check routes through this hook, so hiding here would
  // bail it → empty tiles.
  if (m_capturing)
    return false;
  if (!m_active || !w || mon != m)
    return false;
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

void Overview::deactivate() {
  restoreLayers(); // safety net: normally close() already restored; harmless if
                   // empty
  restoreFill(); // drop the fill-small override so real windows render normally
                 // again
  m_canvasPos.clear();

  if (const auto m = m_monitor.lock()) {
    if (const auto ws = m_workspace.lock(); ws && ws != m->m_activeWorkspace)
      m->changeWorkspace(ws, false, true, false);
  }

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
  m_hovered = m_hoveredStrip = -1;
  m_selected = -1;
  m_recaptureLeft = 0;
  if (m_recaptureTimer)
    m_recaptureTimer->cancel();
  damage();
}

void Overview::damage() const {
  if (const auto m = m_monitor.lock(); m && g_pHyprRenderer)
    g_pHyprRenderer->damageMonitor(m);
}

void Overview::dbg(const std::string &msg) const {
  if (cfgInt("plugin:gloview:debug_logs", 0) != 0 && Log::logger)
    Log::logger->log(Log::INFO, "[gloview] {}", msg);
}

} // namespace gloview
