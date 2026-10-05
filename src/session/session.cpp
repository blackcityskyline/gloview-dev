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
  m_tiles.clear();
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

// config via value(): getConfigValue() ignores Lua config
std::string Overview::cursorMode() const {
    return cfg::behavior.cursor_mode.get();
}

namespace {
// Alpha FIRST (0xAARRGGBB), NOT rgba(RRGGBBAA)
} // namespace

SP<Render::ITexture> Overview::cachedLabel(void *key, const std::string &text,
                                           const CHyprColor &col,
                                           int size, int maxW) const {
  // maxW <= 0 = untruncated. renderText takes a maxWidth, but it clips rather
  // than ellipsizes, so the string is shortened here and re-rendered at the size
  // that fits. Binary search on the character count: the result is cached per
  // key, so this runs once per label per width, not per frame.
  const auto render = [&](const std::string &s) {
    return g_pHyprRenderer->renderText(s, col, size, false, "", 0,
                                       size >= 15 ? 700 : 600);
  };
  if (maxW > 0 && text.size() > 4 && g_pHyprRenderer) {
    const auto probe = render(text);
    if (probe && probe->m_size.x > maxW) {
      int lo = 1, hi = static_cast<int>(text.size()) - 1, best = 0;
      while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        auto t = render(text.substr(0, static_cast<size_t>(mid)) + "\xE2\x80\xA6");
        if (t && t->m_size.x <= maxW) {
          best = mid;
          lo = mid + 1;
        } else {
          hi = mid - 1;
        }
      }
      if (best > 0) {
        const std::string cut = text.substr(0, static_cast<size_t>(best)) +
                                "\xE2\x80\xA6";
        auto &c = m_labelCache[key];
        if (c.text != cut || !c.tex)
          c = model::LabelTex{cut, render(cut)};
        return c.tex;
      }
    }
    // Fits as-is: fall through to the plain path below with the original text.
  }
  auto &c = m_labelCache[key];
  if ((!c.tex || c.text != text) && g_pHyprRenderer)
    c = model::LabelTex{text, render(text)};
  return c.tex;
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

void Overview::toggleAllWorkspaces() {
  if (!(m_active && m_opening)) {
    m_allOverride =
        1; // BEFORE open(), so buildTiles sees it
    open();
    if (!m_active)
      m_allOverride = -1;
    return;
  }
  const bool wasAll = showAllWorkspaces();
  m_allOverride = wasAll ? 0 : 1;
  m_wsSlideDir = 1; // all<->one slides horizontally (no ws order)
  m_expoFlip = wasAll ? -1 : 1;
  auto oldBoxes = captureCurrentBoxes();
  replayReflow(oldBoxes);
}

void Overview::altTabInvoke(bool reverse) {
  if (!(m_active && m_opening)) {
    m_allOverride = 1;
    open();
    if (!m_active) {
      m_allOverride = -1;
      return;
    }
    m_altTabbing = true; // armed AFTER open(): a plain open resets it
    m_selected = m_tiles.empty() ? -1 : 0;
    // carousel slots build around m_selected
    if (carouselMode() && !m_tiles.empty())
      layoutTiles();
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

// canvas never moves or resizes a real window
void Overview::setDesktopMode(bool on) {
  m_desktopMode = on;

  auto oldBoxes = captureCurrentBoxes();
  m_hovered = m_hoveredStrip = -1;
  // The selection survives: free-arrange ignores it, and dropping it left the
  // carousel's relayout refusing to fire on the way back, so the run came back
  // with no centered card and a "0 / N" counter.
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
  m_hovered = m_hoveredStrip = -1;

  // force-rebuild the bg blur cache (boot-built, cold)
  m->m_blurFBDirty = true;

  // force several full-damage frames: one damage() = one image
  m->m_forceFullFrames = 8;

  // clear drag state: mid-drag dismiss skips release
  m_drag = {};
  m_pendingFocus.reset();
  cancelPendingClick();
  m_altTabbing = false;
  m_desktopMode = false;
  m_newCardAnim = false;
  m_newCardId = 0;
  releaseNewWorkspaces();

  buildTiles();
  buildStrip();
  if (m_tiles.empty() && m_strip.size() <= 1)
    return;

  // seed selection BEFORE layoutTiles
  m_selected = m_tiles.empty() ? -1 : 0;
  if (const auto fw = Desktop::focusState()->window()) {
    for (size_t i = 0; i < m_tiles.size(); ++i)
      if (m_tiles[i].win.lock() == fw) {
        m_selected = static_cast<int>(i);
        break;
      }
  }

  layoutTiles();

  m_active = true;
  m_opening = true;
  m_jumpMode = false;
  m_pendingJumpClose = false;
  m_openStamp = std::chrono::steady_clock::now();
  debug::dbg("OPEN tick=" + std::to_string(m_frameTick) +
             " entryToActiveUs=" +
             std::to_string(std::chrono::duration<double, std::micro>(
                                 m_openStamp - openEntryStamp)
                                 .count()));
  m_pendingDeactivate = false;
  m_progress = 0.0;
  m_timeline.begin();
  m_tileClock.begin();
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
  m_opening = false;
  m_pendingJumpClose = false;

  // ANY close makes a pending click timer stale
  cancelPendingClick();

  // close: target = shown, natural = real geometry
  if (const auto m = m_monitor.lock()) {
    // Two passes on purpose. The unfocused cards are pinned to their own box
    // only when their real geometry COLLIDES with the focused card's, and the
    // loop assigns naturals as it goes — so comparing against
    // m_tiles[m_selected].natural inside the same loop reads the value from
    // BEFORE this close, and every card looked distinct and got no pin at all.
    LRect selNat{};
    if (m_selected >= 0 && m_selected < static_cast<int>(m_tiles.size())) {
      if (const auto w = m_tiles[static_cast<size_t>(m_selected)].win.lock()) {
        const auto p = w->positionAnimation()->goal();
        const auto s = w->sizeAnimation()->goal();
        selNat = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                       std::max(1.0, s.x), std::max(1.0, s.y)};
      }
    }
    for (auto &t : m_tiles) {
      t.target = currentBox(t, static_cast<int>(&t - m_tiles.data()));
      // The natural box is where the tile RETURNS TO. When several windows share
      // one geometry — every window maximized looks identical, and so does every
      // window on a workspace that is not the one on screen — their returns are
      // the same rectangle, and the whole run collapses onto it at once: the
      // unfocused cards end up exactly under the focused one, which is drawn
      // last, so their exit is invisible no matter what it is animated to. Give
      // each unfocused card its own exit instead, and let the focused one fly
      // home as the only tile that moves to a real box.
      if (const auto w = t.win.lock()) {
        const auto p = w->positionAnimation()->goal();
        const auto s = w->sizeAnimation()->goal();
        t.natural = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                          std::max(1.0, s.x), std::max(1.0, s.y)};
        if (carouselMode() && static_cast<int>(&t - m_tiles.data()) != m_selected &&
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
  m_timeline.seek(1.0 - m_progress, animDuration());
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
  m_desktopMode = false;
  m_newCardAnim = false;
  m_newCardId = 0;
  m_progress = 0.0;
  m_altTabbing = false;
  m_tiles.clear();
  m_strip.clear();
  m_snapshots.clear();
  m_hovered = m_hoveredStrip = -1;
  m_selected = -1;

  damage();
}

bool Overview::showAllWorkspaces() const {
  return m_allOverride < 0
             ? (cfg::behavior.show_all_workspaces != 0)
             : (m_allOverride != 0);
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

bool Overview::shouldHideWindow(const PHLWINDOW &w,
                                const PHLMONITOR &mon) const {
  const auto m = m_monitor.lock();
  if (!m_active || !w || mon != m)
    return false;
  // fullscreen hidden: a transition force-render leaks it
  if (Fullscreen::controller()->isFullscreen(w))
    return true;
  for (const auto &t : m_tiles)
    if (t.win.lock() == w)
      return true;
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
  // hand the blur FB back: dirty, so preRender rebuilds it
  if (const auto mm = m_monitor.lock())
    mm->m_blurFBDirty = true;
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
  m_desktopMode = false;
  m_allOverride = -1;
  m_altTabbing = false;
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

void Overview::releaseNewWorkspaces() {
  for (const auto &wref : m_newWorkspaces)
    if (const auto ws = wref.lock())
      ws->setPersistent(false);
  m_newWorkspaces.clear();
}

void Overview::damage() const {
  if (const auto m = m_monitor.lock(); m && g_pHyprRenderer)
    g_pHyprRenderer->damageMonitor(m);
}

// EXECUTION tail: m_tiles must survive the frame
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
    if (m_snapshots.contains(w.get()))
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

} // namespace gloview
