#include "../config/config.hpp"
#include "../debug/log.hpp"
#include "../overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <utility>

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {
using PSHOULDRENDER = bool (*)(void *, PHLWINDOW, PHLMONITOR);
PSHOULDRENDER g_shouldRenderOrig = nullptr;

bool hkShouldRenderWindow(void *thisptr, PHLWINDOW window, PHLMONITOR monitor) {
  const bool hide = g_overview && g_overview->shouldHideWindow(window, monitor);
  if (g_overview && g_overview->opening()) {
    debug::dbg("SRW tick=" + std::to_string(g_overview->frameTick()) +
               " win=" + std::to_string(reinterpret_cast<std::uintptr_t>(window.get())) +
               " class=" + (window ? window->m_class : "?") +
               " active=" + std::to_string(g_overview->active()) +
               " hide=" + std::to_string(hide));
  }
  if (hide)
    return false;
  return g_shouldRenderOrig ? g_shouldRenderOrig(thisptr, window, monitor) : true;
}

// commitWindow's "visible" is !isHidden, NOT shouldRenderWindow: a hidden
  // window would still pin the compositor
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
            // live tile previews MUST keep damage (video)
            if (g_overview->snapshotMode() || !g_overview->isTileWindow(w))
              return;
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
  m_active = false;
  m_opening = false;
  restoreLayers();
  restoreFill();
  releaseNewWorkspaces();
  if (m_animPump) { m_animPump->cancel(); m_animPump.reset(); }
  m_scene.clear();
  m_strip.clear();
  cancelPendingClick(); // no timer callback against a half-destroyed Overview
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

  const auto matches =
      HyprlandAPI::findFunctionsByName(m_handle, "shouldRenderWindow");
  void *addr = nullptr;
  for (const auto &mt : matches) {
    // the demangled match names the REAL type, not PHLMONITOR
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
    HyprlandAPI::addNotification(
        m_handle,
        "[gloview] failed to hook shouldRenderWindow — is another gloview "
        "instance loaded (hyprpm)?",
        CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
    return false;
  }
  g_shouldRenderOrig =
      reinterpret_cast<PSHOULDRENDER>(m_shouldRenderHook->m_original);

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

std::string Overview::cursorMode() const {
    return cfg::behavior.cursor_mode.get();
}

namespace {
} // namespace
static constexpr double kKillZoneFalloff = 150.0;

LRect Overview::killZoneRect() const {
  const auto m = m_monitor.lock();
  if (!m)
    return {};
  constexpr double kH = 40.0; // 50 * 0.8: 20% shorter
  const bool atTop = stripAnchor() == Anchor::Bottom;
  return LRect{0.0, atTop ? 0.0 : m->m_size.y - kH, m->m_size.x, kH};
}

// Full brightness inside the zone, nothing by kKillZoneFalloff px away,
// smoothstepped between. A distance, not a bool: a zone blank until the cursor
// crosses it says nothing while the user is still aiming it.
double Overview::killZoneGlow(double lx, double ly) const {
  const LRect z = killZoneRect();
  if (z.w < 1.0 || z.h < 1.0)
    return 0.0;
  const double dx = std::max({z.x - lx, 0.0, lx - (z.x + z.w)});
  const double dy = std::max({z.y - ly, 0.0, ly - (z.y + z.h)});
  const double d  = std::sqrt(dx * dx + dy * dy);
  const double t  = std::clamp(1.0 - d / kKillZoneFalloff, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

Overview::Anchor Overview::stripAnchor() const {
  std::string a = cfg::strip.anchor.get();
  if (a.empty()) // back-compat knob
    a = cfg::strip.bar_position.get();
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
  const double cross = stripHorizontal() ? m->m_size.y : m->m_size.x;
  return std::clamp(
      static_cast<double>(cfg::strip.height), 100.0,
      cross * 0.42);
}

double Overview::stripOffset() const {
  const auto m = m_monitor.lock();
  if (!m)
    return 0.0;
  const double cross = stripHorizontal() ? m->m_size.y : m->m_size.x;
  return std::clamp(
      static_cast<double>(cfg::strip.offset), 0.0,
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

float Overview::blurStrength() const {
  return std::clamp(cfg::blur.strength.get(), 0.0F, 1.0F);
}

int Overview::blurPasses() const {
  return std::clamp(cfg::blur.passes.get(), 1, 16);
}

int Overview::blurSize() const {
  return std::clamp(cfg::blur.size.get(), 1, 200);
}

int Overview::blurResolution() const {
  return std::clamp(cfg::blur.resolution.get(), 1, 32);
}

void Overview::toggle() {
  debug::dbg("TOGGLE tick=" + std::to_string(m_frameTick) +
             " active=" + std::to_string(m_active) +
             " opening=" + std::to_string(m_opening));
  if (m_active && m_opening)
    close();
  else
    open();
}

// Asymmetric on purpose, NOT a copy of the "a" binding: one -> all with the run
// on; all -> all, run only flips. No path back — that is "a"'s job.
void Overview::toggleCarousel() {
  if (!(m_active && m_opening)) {
    m_view.setCarouselOn( true); // BEFORE open(): buildTiles reads it
    m_view.setAllOverride( 1);
    open();
    if (!m_active)
      m_view.setAllOverride( -1);
    return;
  }
  if (!showAllWorkspaces()) {
    m_view.setCarouselOn( true); // BEFORE replayReflow -> layoutTiles reads it
    toggleAllWorkspaces();
    return;
  }
  m_view.setCarouselOn( !m_view.carouselOn());
  // The SAME rebuild the 'a' binding does: a bare layoutTiles() only rewrites
  // targets, so the carousel era's boxes survive and the grid is scattered.
  m_view.setWsSlideDir( 0); // the view is not changing, so no slide
  m_view.setExpoFlip( 0);
  auto oldBoxes = captureCurrentBoxes();
  replayReflow(oldBoxes);
  debug::dbg("TOGGLE-CAROUSEL on=" + std::to_string(m_view.carouselOn()));
}

void Overview::toggleAllWorkspaces() {
  if (!(m_active && m_opening)) {
    m_view.setAllOverride(
        1); // BEFORE open(), so buildTiles sees it
    open();
    if (!m_active)
      m_view.setAllOverride( -1);
    return;
  }
  const bool wasAll = showAllWorkspaces();
  m_view.setAllOverride( wasAll ? 0 : 1);
  m_view.setWsSlideDir( 1); // all<->one slides horizontally (no ws order)
  m_view.setExpoFlip( wasAll ? -1 : 1);
  auto oldBoxes = captureCurrentBoxes();
  replayReflow(oldBoxes);
}

void Overview::altTabInvoke(bool reverse) {
  if (!(m_active && m_opening)) {
    // Armed BEFORE open() and consumed by the layout it triggers, so the entry is
    // aimed from its first frame: ONE motion of ONE duration.
    m_pendingStep = cfg::behavior.alttab_advances != 0 ? (reverse ? -1 : 1) : 0;
    m_view.setAllOverride( 1);
    open();
    m_pendingStep = 0;
    if (!m_active) {
      m_view.setAllOverride( -1);
      return;
    }
    m_altTabbing = true; // armed AFTER open(): a plain open resets it
    // open() already seeded the selection to the window that had the compositor
    // focus. Selecting index 0 here threw that away and landed on whatever window
    // happened to be first in the scene, which is why the same window came up on
    // every invocation.
    if (carouselMode() && !m_scene.empty())
      layoutTiles();
    damage();
    return;
  }
  stepAltTab(reverse ? -1 : 1);
}

void Overview::toggleDesktop() {
  if (m_active && m_opening)
    setDesktopMode(!m_view.desktop());
}

// canvas never moves or resizes a real window
void Overview::setDesktopMode(bool on) {
  m_view.setDesktop( on);

  auto oldBoxes = captureCurrentBoxes();
  m_scene.clearHover();
  // Selection survives: dropping it stopped the carousel relayout coming back.
  replayReflow(oldBoxes);
}

void Overview::open() {
  const auto openEntryStamp = std::chrono::steady_clock::now();
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
      m->m_activeWorkspace;
  m_scene.clearHover();

  m->m_blurFBDirty = true;

  // force several full-damage frames: one damage() = one image
  m->m_forceFullFrames = 8;

  m_drag = {};
  m_pendingFocus.reset();
  cancelPendingClick();
  m_altTabbing = false;
  m_view.setDesktop( false);
  m_newCardAnim = false;
  m_newCardId = 0;
  releaseNewWorkspaces();

  buildTiles();
  buildStrip();
  if (m_scene.tiles().empty() && m_strip.size() <= 1)
    return;

  // seed selection BEFORE layoutTiles
  m_scene.select(m_scene.tiles().empty() ? -1 : 0);
  if (const auto fw = Desktop::focusState()->window()) {
    for (size_t i = 0; i < m_scene.tiles().size(); ++i)
      if (m_scene.tiles()[i].win.lock() == fw) {
        m_scene.select(static_cast<int>(i));
        break;
      }
  }

  layoutTiles();

  m_view.setCarouselOn( cfg::carousel.on_open != 0);
  m_active = true;
  m_opening = true;
  m_view.setJumpMode( false);
  m_pendingJumpClose = false;
  m_openStamp = std::chrono::steady_clock::now();
  debug::dbg("OPEN tick=" + std::to_string(m_frameTick) +
             " entryToActiveUs=" +
             std::to_string(std::chrono::duration<double, std::micro>(
                                 m_openStamp - openEntryStamp)
                                 .count()));
  m_pendingDeactivate = false;
  m_progress = 0.0;
  // Immersive fade only when the desktop underneath has no windows: with windows, their
  // Hyprland blur is what covers the gap the ramp would open.
  m_landingEmpty = !workspaceHasWindows(m_liveWsAtOpen.lock());
  debug::dbg("open: ws=" + std::to_string(m_liveWsAtOpen.lock() ? m_liveWsAtOpen.lock()->m_id : -1) + " empty=" + std::to_string(m_landingEmpty ? 1 : 0));
  m_clocks.master.begin();
  m_clocks.tiles.begin();
  m_clocks.blurFade.begin();
  m_lastAnimTick = {};
  hideLayers();
  m_cursor.onOpen(m, cursorMode());
  static constexpr auto kBackdropCacheTTL = std::chrono::seconds(3);
  const bool backdropStale =
      (std::chrono::steady_clock::now() - m_lastBackdropCapture) >
      kBackdropCacheTTL;
  if (backdropStale) {
    m_backdropDrawn = false;
    // do NOT free the blur cache (it backs translucent tiles)
    m_blur.valid = false;
  }
  debug::dbg("=== OPEN ===");
  damage();
}

void Overview::close() {
  if (!m_active)
    return;
  // Symmetric exit: the ramp runs back DOWN over the same blur_entry_fade instead of
  // riding the close animation. pow(k, 0.45) held the blur above 0.6 for ten of thirteen
  // frames and then dropped 0.475 -> 0.043 in the last two — a hard cut, not a fade.
  m_blurFade0 = blurAlpha();
  // Re-read: the landing workspace can have changed while the overview was up, and it is
  // THIS one we land back on, so it decides whether the exit may be a slow fade.
  m_landingEmpty = !workspaceHasWindows(m_liveWsAtOpen.lock());
  // Ask Hyprland for the frames in which it rebuilds its own blur, and keep the overlay
  // up until they are spent: blur under a still-moving window is not rendered yet, so
  // releasing earlier just uncovers a sharp desktop.
  if (const auto m = m_monitor.lock())
    m->m_forceFullFrames = std::max(m->m_forceFullFrames, 8);
  debug::dbg("close: ws=" + std::to_string(m_workspace.lock() ? m_workspace.lock()->m_id : -1) + " empty=" + std::to_string(m_landingEmpty ? 1 : 0));
  m_clocks.blurFade.begin();
  m_opening = false;
  m_pendingJumpClose = false;

  cancelPendingClick();

  // close: target = shown, natural = real geometry
  if (const auto m = m_monitor.lock()) {
    // Two passes on purpose: the pin test needs m_scene.tiles()[m_scene.selected()].natural, which
    // the loop rewrites as it goes — testing inside it reads the pre-close value.
    LRect selNat{};
    if (m_scene.selected() >= 0 && m_scene.selected() < static_cast<int>(m_scene.tiles().size())) {
      if (const auto w = m_scene.tiles()[static_cast<size_t>(m_scene.selected())].win.lock()) {
        const auto p = w->positionAnimation()->goal();
        const auto s = w->sizeAnimation()->goal();
        selNat = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                       std::max(1.0, s.x), std::max(1.0, s.y)};
      }
    }
    for (auto &t : m_scene.edit()) {
      t.target = currentBox(t, static_cast<int>(&t - m_scene.tiles().data()));
      // The natural box is where the tile RETURNS TO. When several windows share
      // one geometry — maximized windows, off-screen workspaces — the unfocused
      // cards land under the focused one (drawn last), so their exit is invisible
      // whatever it animates to. Each gets its own exit.
      if (const auto w = t.win.lock()) {
        const auto p = w->positionAnimation()->goal();
        const auto s = w->sizeAnimation()->goal();
        t.natural = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                          std::max(1.0, s.x), std::max(1.0, s.y)};
        if (carouselMode() && static_cast<int>(&t - m_scene.tiles().data()) != m_scene.selected() &&
            selNat.w >= 1.0 && std::abs(t.natural.x - selNat.x) < 1.0 &&
            std::abs(t.natural.y - selNat.y) < 1.0 &&
            std::abs(t.natural.w - selNat.w) < 1.0 &&
            std::abs(t.natural.h - selNat.h) < 1.0)
          t.natural = t.target; // leaves by shrinking in place, not by vanishing
      }
    }
  }

  restoreLayers();
  m_cursor.onClose();
  // resume from CURRENT progress (1 - raw())
  m_clocks.master.seek(1.0 - m_progress, animDuration());
  m_lastAnimTick = {}; // else a phantom gap rewinds the clocks
  debug::dbg("=== CLOSE from progress " + std::to_string(m_progress).substr(0, 5) + " ===");
  damage();
}

// sync teardown for unload: close() only STARTS it; unload first -> SEGV
void Overview::hardClose() {
  cancelPendingClick();
  if (m_animPump) {
    m_animPump->cancel();
    m_animPump.reset();
  }

  restoreLayers();
  restoreFill();
  releaseNewWorkspaces();

  m_active = false;
  m_opening = false;
  m_pendingDeactivate = false;
  m_view.setDesktop( false);
  m_newCardAnim = false;
  m_newCardId = 0;
  m_progress = 0.0;
  m_altTabbing = false;
  m_scene.clear();
  m_strip.clear();
  m_pixel.clearSnapshots();
  m_scene.clearHover();
  m_scene.select(-1);
  m_focusOutside.reset();

  damage();
}

bool Overview::showAllWorkspaces() const {
  return m_view.allOverride() < 0
             ? (cfg::behavior.show_all_workspaces != 0)
             : (m_view.allOverride() != 0);
}

// buildTiles() and syncTiles() MUST share this
bool Overview::tileBelongs(const PHLWINDOW &w, const PHLMONITOR &m,
                           const PHLWORKSPACE &ws) const {
  if (!w || !w->m_isMapped || w->isHidden())
    return false;
  const auto wws = w->m_workspace;
  if (!wws)
    return false;
  if (showAllWorkspaces()) {
    if (wws->m_isSpecialWorkspace &&
        cfg::behavior.show_special == 0)
      return false;
    return wws->m_monitor.lock() == m;
  }
  return wws == ws;
}

bool Overview::workspaceHasWindows(const PHLWORKSPACE &ws) {
  if (!ws)
    return false;
  for (const auto &w : Desktop::windowState()->windows())
    if (w && w->m_isMapped && !w->isHidden() && w->m_workspace == ws)
      return true;
  return false;
}

bool Overview::shouldHideWindow(const PHLWINDOW &w,
                                const PHLMONITOR &mon) const {
  const auto m = m_monitor.lock();
  if (!m_active || !w || mon != m)
    return false;
  // fullscreen hidden: a transition force-render leaks it
  if (Fullscreen::controller()->isFullscreen(w))
    return true;
  for (const auto &t : m_scene.tiles())
    if (t.win.lock() == w)
      return true;
  if (const auto aw = m->m_activeWorkspace; aw && w->m_workspace == aw)
    return true;
  return false;
}

bool Overview::isTileWindow(const PHLWINDOW &w) const {
    if (!w)
        return false;
    for (const auto &t : m_scene.tiles())
        if (t.win.lock() == w)
            return true;
    return false;
}

// Feeds the recency service from the COMPOSITOR's focus, and only while the
// overview is closed. Inside, the run's selection is internal, so nothing here may
// read it: promoting on an internal hover would make the order depend on where the
// pointer happened to be. Called every frame and deliberately cheap — one pointer
// compare.
void Overview::syncRecency() {
  if (m_active)
    return;
  const auto now = Desktop::focusState()->window();
  if (now == m_lastFocus)
    return;
  m_lastFocus = now;
  if (now)
    m_recency.promote(now.get());
}

int Overview::tileOf(const PHLWINDOW &w) const {
  for (size_t i = 0; i < m_scene.size(); ++i)
    if (m_scene.tiles()[i].win.lock() == w)
      return static_cast<int>(i);
  return -1;
}

// The recency service is a cache of the live window set and must never outlive it.
// Hyprland reuses window addresses, so an entry for a closed window would hand its
// rank to an unrelated window that lands on the same address.
void Overview::pruneRecency() {
  std::vector<const void *> live;
  live.reserve(m_scene.size());
  for (const auto &t : m_scene.tiles())
    live.push_back(t.win.lock().get());
  m_recency.forgetDead(live);
}

void Overview::deactivate() {
  if (const auto mm = m_monitor.lock()) {
    mm->m_blurFBDirty = true;
    // The window is still flying into its tile here, so Hyprland's own blur is not
    // rendering yet: for the first frames after the overlay goes the desktop shows
    // through unblurred. Force the full-damage frames open() asks for, or the blur FB
    // is not rebuilt in time and the exit pops.
    mm->m_forceFullFrames = 8;
  }
  restoreLayers();
  restoreFill();

  if (const auto m = m_monitor.lock()) {
    if (const auto ws = m_workspace.lock(); ws && ws != m->m_activeWorkspace) {
      const auto oldWs = m->m_activeWorkspace; // capture BEFORE the switch
      m->changeWorkspace(ws, false, true, false);
      // changeWorkspace always starts the native slide; warp to goal
      if (oldWs && oldWs != ws) {
        oldWs->m_alpha->setValueAndWarp(oldWs->m_alpha->goal());
        oldWs->m_renderOffset->setValueAndWarp(oldWs->m_renderOffset->goal());
      }
      ws->m_alpha->setValueAndWarp(ws->m_alpha->goal());
      ws->m_renderOffset->setValueAndWarp(ws->m_renderOffset->goal());
    }
  }

  // re-assert committed focus LAST (stale auto-focus)
  if (const auto pw = m_pendingFocus.lock(); pw && pw->m_isMapped && !pw->isHidden())
    Desktop::focusState()->fullWindowFocus(pw, Desktop::FOCUS_REASON_KEYBIND);
  m_pendingFocus.reset();

  // release EVERY created workspace
  debug::dbg("deactivate (natural close end)");
  if (m_animPump) {
    m_animPump->cancel();
    m_animPump.reset();
  }
  releaseNewWorkspaces();

  m_active = false;
  m_opening = false;
  m_pendingDeactivate = false;
  m_view.setDesktop( false);
  m_view.setAllOverride( -1);
  m_altTabbing = false;
  m_newCardAnim = false;
  m_newCardId = 0;
  m_progress = 0.0;
  m_scene.clear();
  m_strip.clear();
  m_pixel.clearSnapshots();
  m_scene.clearHover();
  m_scene.select(-1);
  damage();
}

void Overview::releaseNewWorkspaces() {
  for (const auto &wref : m_fx.newWorkspaces())
    if (const auto ws = wref.lock())
      ws->setPersistent(false);
  m_fx.clearNewWorkspaces();
}

void Overview::damage() const {
  if (const auto m = m_monitor.lock(); m && g_pHyprRenderer)
    g_pHyprRenderer->damageMonitor(m);
}

// EXECUTION tail: m_scene.tiles() must survive the frame
void Overview::finishPendingDeactivate() {
  if (!m_pendingDeactivate)
    return;
  m_pendingDeactivate = false;
  debug::dbg("handoff: final overlay frame painted, deactivating");
  deactivate();
}

void Overview::rearmanim() const {
  if (!animBusy())
    return;
  if (g_pEventLoopManager)
    const_cast<Overview *>(this)->ensureAnimPump();
}

void Overview::ensureAnimPump() {
  if (!animBusy()) {
    if (m_animPump) {
      m_animPump->cancel();
      m_animPump.reset();
    }
    return;
  }
  if (m_animPump && m_animPump->armed())
    return;

  // tick BETWEEN frames, not during a commit
  m_animPump = makeShared<CEventLoopTimer>(
      std::chrono::milliseconds(8),
      [this](SP<CEventLoopTimer> self, void *) {
        if (!animBusy()) {
          self->cancel();
          return;
        }
        damage();
        if (const auto m = m_monitor.lock())
          m->scheduleFrame();
        self->updateTimeout(std::chrono::milliseconds(8));
      },
      nullptr);
  if (g_pEventLoopManager)
    g_pEventLoopManager->addTimer(m_animPump);
}

// snapshot mode: per-window last committed texture, frozen
bool Overview::snapshotMode() const {
  return cfg::behavior.preview_mode.get() == "snapshot";
}

void Overview::updateSnapshots() {
  if (!snapshotMode())
    return;
  const auto grab = [this](const PHLWINDOW &w) {
    if (!w || !w->wlSurface() || !w->wlSurface()->resource())
      return;
    if (m_pixel.snapshot(w.get()))
      return;
    const auto tex = w->wlSurface()->resource()->m_current.texture;
    if (!tex || !tex->ok())
      return;
    // live texture is re-uploaded per commit: copy to a private FB
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

    m_pixel.putSnapshot(w.get(), fb->getTexture());
  };
  for (const auto &t : m_scene.tiles())
    if (const auto w = t.win.lock())
      grab(w);
  for (const auto &it : m_strip)
    for (const auto &sw : it.wins)
      if (const auto w = sw.win.lock())
        grab(w);
}

} // namespace gloview
