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
#include <hyprland/src/state/WorkspaceState.hpp>
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

// --- input routing ---------------------------------------------------------
//
// shouldRenderWindow hides the real windows, but it hides them from the RENDERER
// only. CSeatManager still hands them pointer and keyboard focus, so a window under
// the cursor receives enter and motion while the overview is on screen, and a keypress
// lands in whatever Hyprland happened to have focused. Both are invisible to
// shouldRenderWindow and neither is reachable through Event::bus(): focus is not an
// event the bus carries.
//
// These are the two functions where input BECOMES addressed, which is why they are
// hooked rather than the send* calls: blocking a send leaves the seat believing the
// window still has focus, and the compositor keeps routing to it.
using PSETKEYFOCUS = void (*)(void *, SP<CWLSurfaceResource>);
using PSETPTRFOCUS = void (*)(void *, SP<CWLSurfaceResource>, const Vector2D &);
PSETKEYFOCUS g_setKeyboardFocusOrig = nullptr;
PSETPTRFOCUS g_setPointerFocusOrig = nullptr;

// True while the overview owns the screen. m_pendingDeactivate is deliberately NOT
// included: the final frame still draws the overlay, and by then the desktop is
// expected to be interactive again. openedOrParked() rather than active(), because the
// parked frames are already the overview's -- the overlay is merely not drawn yet, and
// input must not reach the windows it is covering.
bool inputRoutedAway() {
  return g_overview && g_overview->openedOrParked();
}

// The hooks refuse to move focus to a window, but they do not drop focus from the one
// that already had it. Without this the window under the cursor keeps receiving motion
// for the whole session. refocus() re-runs Hyprland's own resolution, which now lands
// on nullptr because the hooks are in the way -- and lands on the real window again once
// the overview is gone, so no separate restore path is needed.
void dropFocusFromWindows() {
  if (g_pInputManager)
    g_pInputManager->refocus(std::nullopt);
}

void hkSetKeyboardFocus(void *thisptr, SP<CWLSurfaceResource> surf) {
  if (inputRoutedAway()) {
    // nullptr is Hyprland's own "no window has focus" value, so the seat ends up in
    // the state it would be in with nothing to focus, rather than keeping a stale one.
    g_setKeyboardFocusOrig(thisptr, nullptr);
    return;
  }
  g_setKeyboardFocusOrig(thisptr, surf);
}

void hkSetPointerFocus(void *thisptr, SP<CWLSurfaceResource> surf,
                       const Vector2D &local) {
  if (inputRoutedAway()) {
    g_setPointerFocusOrig(thisptr, nullptr, local);
    return;
  }
  g_setPointerFocusOrig(thisptr, surf, local);
}

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
  restorePreviewVisibility();
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
  if (m_setPointerFocusHook) {
    HyprlandAPI::removeFunctionHook(m_handle, m_setPointerFocusHook);
    m_setPointerFocusHook = nullptr;
  }
  if (m_setKeyboardFocusHook) {
    HyprlandAPI::removeFunctionHook(m_handle, m_setKeyboardFocusHook);
    m_setKeyboardFocusHook = nullptr;
  }
  g_shouldRenderOrig = nullptr;
  g_damageSurfaceOrig = nullptr;
  g_setPointerFocusOrig = nullptr;
  g_setKeyboardFocusOrig = nullptr;
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

    // Input routing. Both are required: half of this is worse than none, because the
  // overview would look modal for the keyboard and not for the mouse.
  struct FocusHook {
    const char *name;
    const char *sig;
    void *hookFn;
    CFunctionHook **slot;
    void **orig;
  };
  const FocusHook focusHooks[] = {
      {"setPointerFocus",
       "setPointerFocus(Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>, "
       "Hyprutils::Math::Vector2D const&)",
       reinterpret_cast<void *>(&hkSetPointerFocus),
       &m_setPointerFocusHook, reinterpret_cast<void **>(&g_setPointerFocusOrig)},
      {"setKeyboardFocus", "setKeyboardFocus(Hyprutils::Memory::"
                           "CSharedPointer<CWLSurfaceResource>)",
       reinterpret_cast<void *>(&hkSetKeyboardFocus),
       &m_setKeyboardFocusHook,
       reinterpret_cast<void **>(&g_setKeyboardFocusOrig)},
  };
  for (const auto &fh : focusHooks) {
    void *addrFocus = nullptr;
    for (const auto &mt :
         HyprlandAPI::findFunctionsByName(m_handle, fh.name)) {
      if (mt.demangled.find(fh.sig) != std::string::npos) {
        addrFocus = mt.address;
        break;
      }
    }
    if (!addrFocus) {
      HyprlandAPI::addNotification(
          m_handle,
          std::string("[gloview] could not find ") + fh.name +
              " to hook — input will reach the windows under the overview",
          CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
      return false;
    }
    *fh.slot = HyprlandAPI::createFunctionHook(m_handle, addrFocus, fh.hookFn);
    if (!*fh.slot || !(*fh.slot)->hook()) {
      *fh.slot = nullptr;
      HyprlandAPI::addNotification(
          m_handle, std::string("[gloview] failed to hook ") + fh.name,
          CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
      return false;
    }
    *fh.orig = (*fh.slot)->m_original;
  }
  return true;
}

std::string Overview::cursorMode() const {
    return cfg::behavior.cursor_mode.get();
}

namespace {
} // namespace
static double kKillZoneFalloff() { return cfg::behavior.kill_zone_falloff; }

LRect Overview::killZoneRect() const {
  const auto m = m_monitor.lock();
  if (!m)
    return {};
  if (cfg::behavior.kill_zone == 0)
    return {}; // disabled: an empty rect contains nothing and never closes a window
  const double kH = cfg::behavior.kill_zone_size;
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
  const double t  = std::clamp(1.0 - d / kKillZoneFalloff(), 0.0, 1.0);
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
      cross * cfg::strip.offset_max);
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
  const double d = (1.0 - e) * stripThickness() * cfg::strip.slide_frac;
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

// The band yields to a preview whose edge has reached it: the penetration depth becomes an
// outward displacement, so contact reads as the window PUSHING the band instead of sliding
// under it (measured before: the preview simply overrode the band and the two read as one
// overlapping blob). The deepest preview wins -- the band is a single object. Capped at the
// band's own thickness so a fullscreen window shoves it out of reach instead of dragging it
// across the screen.
// Generic yield, for chrome that is not the strip: displaced along the SHORTEST way out by
// the overlap depth, so contact reads as the preview pushing it rather than drawing over it.
// Deepest preview wins. The distance is bounded by the element's own size on the escape axis,
// so nothing is ever dragged across the screen and no cap is needed.
Vector2D Overview::pushOut(const LRect &r) const {
  double best = 0.0;
  Vector2D dir{0.0, 0.0};
  const auto &tiles = m_scene.tiles();
  for (size_t i = 0; i < tiles.size(); ++i) {
    const LRect b = currentBox(tiles[i], static_cast<int>(i));
    if (b.w < 1.0 || b.h < 1.0)
      continue;
    const double ox = std::min(b.x + b.w, r.x + r.w) - std::max(b.x, r.x);
    const double oy = std::min(b.y + b.h, r.y + r.h) - std::max(b.y, r.y);
    if (ox <= 0.0 || oy <= 0.0)
      continue; // not touching

    // SWALLOWED: the preview covers the element whole, so there is no cheaper axis to leave
    // by and both overlaps equal the element's size. Eject toward the nearest edge instead,
    // otherwise the displacement degenerates to one element-sized step and stops there -- the
    // measured "nudges once, then stands still while the window draws over it".
    Vector2D d{0.0, 0.0};
    double depth = 0.0;
    if (ox >= r.w && oy >= r.h) {
      double near = r.x - b.x;
      d = Vector2D{-1.0, 0.0};
      if (b.x + b.w - (r.x + r.w) < near) {
        near = b.x + b.w - (r.x + r.w);
        d = Vector2D{1.0, 0.0};
      }
      if (r.y - b.y < near) {
        near = r.y - b.y;
        d = Vector2D{0.0, -1.0};
      }
      if (b.y + b.h - (r.y + r.h) < near) {
        near = b.y + b.h - (r.y + r.h);
        d = Vector2D{0.0, 1.0};
      }
      depth = near;
    } else if (ox < oy) {
      depth = ox;
      d = Vector2D{std::copysign(1.0, r.cx() - b.cx()), 0.0};
    } else {
      depth = oy;
      d = Vector2D{0.0, std::copysign(1.0, r.cy() - b.cy())};
    }
    if (depth > best) {
      best = depth;
      dir = d;
    }
  }
  if (best <= 0.0)
    return {};
  return Vector2D{dir.x * best, dir.y * best};
}

Vector2D Overview::stripOffset(double e) const {
  return stripSlide(e) + stripPush(stripBand());
}

Vector2D Overview::stripPush(const LRect &band) const {
  double deepest = 0.0;
  const auto &tiles = m_scene.tiles();
  for (size_t i = 0; i < tiles.size(); ++i) {
    const LRect b = currentBox(tiles[i], static_cast<int>(i));
    if (b.w < 1.0 || b.h < 1.0)
      continue;
    double pen = 0.0;
    switch (stripAnchor()) {
    case Anchor::Bottom: pen = (b.y + b.h) - band.y; break;
    case Anchor::Top:    pen = (band.y + band.h) - b.y; break;
    case Anchor::Left:   pen = (band.x + band.w) - b.x; break;
    case Anchor::Right:  pen = (b.x + b.w) - band.x; break;
    }
    deepest = std::max(deepest, pen);
  }
  if (deepest <= 0.0)
    return {};
  // Uncapped: the depth is bounded by the tile, and a strip shoved past the screen edge is
  // exactly "задвигает". The cap this replaces held it to one band thickness, which cleared
  // the band for a shallow overlap and left the workspace cards — riding the same slide —
  // painted over the tile edge once the window grew to cover the strip (measured on exit).
  const double d = deepest;
  switch (stripAnchor()) {
  case Anchor::Bottom: return Vector2D{0.0, d};
  case Anchor::Top:    return Vector2D{0.0, -d};
  case Anchor::Left:   return Vector2D{-d, 0.0};
  case Anchor::Right:  return Vector2D{d, 0.0};
  }
  return {};
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

// The close lands on m_workspace, which focusAndClose re-points at the focused window's
// workspace when one is committed. So "will this window be there" is exactly "is it on
// m_workspace" -- one comparison, no policy of its own; anim::tileExit holds the policy.
bool Overview::tileGoesHome(const model::Tile &t) const {
  const auto w    = t.win.lock();
  const auto land = m_workspace.lock();
  if (!w || !land)
    return true; // nothing to compare against: keep the flight we have always done
  return w->m_workspace == land;
}

// The compositor decides what to draw from three pieces of state, and for a window that is
// not on the ACTIVE workspace all three say "no": the workspace is faded out, the window's
// fullscreen alpha is 0, and the window's surfaces carry a visibleRegion left over from
// whenever they were last on screen. The live preview then reads a resource with no
// committed buffer and a geometry that was never given the layout.
//
// ScrollOverview clears all three before drawing a preview and puts them back afterwards.
// Same three, same calls -- forceWorkspaceAlphaVisible / forceWindowVisible /
// forceWindowSurfaceVisibility.
void Overview::forcePreviewVisibility() {
  for (const auto &wsref : State::workspaceState()->workspaces()) {
    const auto ws = wsref.lock();
    if (!ws || !ws->m_alpha)
      continue;
    bool seen = false;
    for (const auto &e : m_forcedAlphaWs)
      if (e.ws.get() == ws.get()) { seen = true; break; }
    if (!seen) {
      ForcedWsAlpha e;
      e.ws  = ws;
      e.val = ws->m_alpha->value();
      m_forcedAlphaWs.push_back(e);
    }
    ws->m_alpha->setValueAndWarp(1.F);
    *ws->m_alpha = 1.F;
  }

  const auto forceWindow = [this](const PHLWINDOW &w) {
    if (!w)
      return;
    constexpr auto FS_ALPHA = Desktop::View::WINDOW_ALPHA_FULLSCREEN;
    bool wseen = false;
      for (const auto &e : m_forcedWindows)
        if (e.w.get() == w.get()) { wseen = true; break; }
      if (!wseen) {
        ForcedWinVis e;
        e.w        = w;
        e.alphaVal = w->alpha(FS_ALPHA)->value();
        m_forcedWindows.push_back(e);
      }
    if (const auto& a = w->alpha(FS_ALPHA))
      a->setValueAndWarp(1.F);
    if (!w->wlSurface() || !w->wlSurface()->resource())
      return;
    w->wlSurface()->resource()->breadthfirst(
        [this](SP<CWLSurfaceResource> s, const Vector2D &, void *) {
          if (!s)
            return;
          const auto HL = Desktop::View::CWLSurface::fromResource(s);
          if (!HL)
            return;
          bool seen = false;
          for (const auto &e : m_forcedRegions)
            if (e.hl.get() == HL.get()) { seen = true; break; }
          if (!seen) {
            ForcedRegion e;
            e.hl     = HL;
            e.region = HL->m_visibleRegion;
            m_forcedRegions.push_back(e);
          }
          HL->m_visibleRegion = {};
        },
        nullptr);
  };

  for (const auto &t : m_scene.tiles())
    forceWindow(t.win.lock());
  for (const auto &it : m_strip)
    for (const auto &sw : it.wins)
      forceWindow(sw.win.lock());
}

void Overview::restorePreviewVisibility() {
  for (const auto &[ws, val] : m_forcedAlphaWs)
    if (ws && ws->m_alpha)
      ws->m_alpha->setValueAndWarp(val);
  m_forcedAlphaWs.clear();

  for (const auto &[w, aVal] : m_forcedWindows) {
    if (!w)
      continue;
    constexpr auto FS_ALPHA = Desktop::View::WINDOW_ALPHA_FULLSCREEN;
    if (const auto& a = w->alpha(FS_ALPHA))
      *a = aVal;
  }
  m_forcedWindows.clear();

  for (const auto &[hl, region] : m_forcedRegions)
    if (hl)
      hl->m_visibleRegion = region;
  m_forcedRegions.clear();
}

void Overview::toggle() {
  debug::dbg("TOGGLE tick=" + std::to_string(m_frameTick) +
             " active=" + std::to_string(m_active) +
             " opening=" + std::to_string(m_opening));
  // While the overview is up the toggle means "go to what I am looking at", the same thing
  // Enter means. It used to close unconditionally, so the one bind the user reaches for
  // first either dropped them on the workspace they started from or -- once the deck held
  // windows from elsewhere -- animated a focus it then cancelled.
  switch (anim::toggleIntent(m_active && m_opening, m_scene.focused() >= 0)) {
    case anim::DismissIntent::Open:
      open();
      break;
    case anim::DismissIntent::Commit:
      activateSelection();
      break;
    case anim::DismissIntent::Cancel:
      close();
      break;
  }
}

// Asymmetric on purpose, NOT a copy of the "a" binding: one -> all with the run
// on; all -> all, run only flips. No path back — that is "a"'s job.
void Overview::toggleCarousel() {
  if (!(m_active && m_opening)) {
    m_view.setCarouselOn( true); // BEFORE open(): buildTiles reads it
    m_view.setAllOverride( 1);
    open();
    if (!openedOrParked())
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
    if (!openedOrParked())
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
    if (!openedOrParked()) {
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
  // Before anything else: make every workspace, and every window we may draw, eligible to
  // be rendered. Taken from ScrollOverview (yayuuu/hyprland-scroll-overview), which solves
  // exactly this and does it here:
  //
  //   * an inactive workspace's windows carry no committed buffer and a default
  //     geometry (measured: three windows of one scrolling workspace all reporting
  //     pos=6,6 size=1354, buf=0), so the live preview reads a texture that is not theirs
  //     -- a blank card, a flicker on every border crossing, a neighbour's pixels on the
  //     hovered one;
  //   * a window's HLSurface keeps a visibleRegion computed for the workspace it was last
  //     drawn on, and the surface pass clips against it, so a card draws nothing.
  //
  // The alternative was asking the compositor to render inactive workspaces: that crashes
  // it inside endRender -> useFP16 -> println (hyprlandCrashReport389377 and 603004). This
  // touches no compositor hook and moves no focus.
  forcePreviewVisibility();

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
  // Parked, NOT active: the pass computes the backdrop privately (primeBackdrop) and flips
  // m_active the moment a non-black composite exists. Going visible now is what put a
  // backdrop-less frame on screen.
  m_pendingOpen = true;
  m_pendingTicks = 0;
  m_active  = false;
  m_opening = false;
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
  // Parked already counts as owned (see inputRoutedAway), so the real windows lose
  // focus on the first parked frame, not when the overlay actually appears.
  dropFocusFromWindows();
  // Immersive fade only when the desktop underneath has no windows: with windows, their
  // Hyprland blur is what covers the gap the ramp would open.
  m_landingEmpty = !workspaceHasWindows(m_liveWsAtOpen.lock());
  debug::dbg("open: ws=" + std::to_string(m_liveWsAtOpen.lock() ? m_liveWsAtOpen.lock()->m_id : -1) + " empty=" + std::to_string(m_landingEmpty ? 1 : 0));
  m_clocks.master.begin();
  m_clocks.tiles.begin();
  m_lastAnimTick = {};
  // hideLayers() is deliberately NOT called here: it runs at promotion instead. Fading the
  // layers now would leave the desktop bare on frames where the overview draws nothing.
  m_cursor.onOpen(m, cursorMode());
  // The backdrop composite is kept and never expires here. renderBackdropSource compares the
  // source texture against the one the composite was made from and redraws when it really
  // changed; it must not be reset by a timer, because right after an idle the wallpaper
  // texture is mid-recreation and any from-scratch composite of it is black.
  debug::dbg("=== OPEN ===");
  damage();
}

void Overview::close() {
  if (m_pendingOpen) { // never became visible, so there is nothing to animate away
    m_pendingOpen = false;
    m_pendingTicks = 0;
    return;
  }
  if (!m_active)
    return;
  // The landing workspace can have changed while the overview was up (all-workspaces,
  // digit jump), and it is THIS one we land back on — re-read it, do not reuse open()'s.
  m_landingEmpty = !workspaceHasWindows(m_workspace.lock());
  // Ask Hyprland for the frames in which it rebuilds its own blur, and keep the overlay
  // up until they are spent: blur under a still-moving window is not rendered yet, so
  // releasing earlier just uncovers a sharp desktop.
  if (const auto m = m_monitor.lock())
    m->m_forceFullFrames = std::max(m->m_forceFullFrames, 8);
  debug::dbg("close: ws=" + std::to_string(m_workspace.lock() ? m_workspace.lock()->m_id : -1) + " empty=" + std::to_string(m_landingEmpty ? 1 : 0));
  m_opening = false;
  m_pendingJumpClose = false;

  cancelPendingClick();

  // close: target = shown, natural = real geometry
  if (const auto m = m_monitor.lock()) {
    // Two passes on purpose: the pin test needs the focused tile's natural, which
    // the loop rewrites as it goes — testing inside it reads the pre-close value.
    LRect selNat{};
    if (m_scene.focused() >= 0) {
      if (const auto w = m_scene.tiles()[static_cast<size_t>(m_scene.focused())].win.lock()) {
        const auto p = w->positionAnimation()->goal();
        const auto s = w->sizeAnimation()->goal();
        selNat = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                       std::max(1.0, s.x), std::max(1.0, s.y)};
      }
    }
    for (auto &t : m_scene.edit()) {
      const int i = static_cast<int>(&t - m_scene.tiles().data());
      t.target = currentBox(t, i);
      // The natural box is where the tile RETURNS TO -- so it is only a destination when
      // the window will really be there. Opening on an empty workspace, switching to all
      // and pressing Escape leaves nothing on the landing workspace: every card's natural
      // box then belonged to a workspace that is not coming, and the exit spent itself
      // flying each one toward a tile it could never occupy before dropping it in a
      // single frame. Those cards leave standing and dissolve instead (entryFade).
      if (const auto w = t.win.lock()) {
        if (anim::tileExit(tileGoesHome(t)) == anim::TileExit::InPlace) {
          t.natural = t.target;
          continue;
        }
        const auto p = w->positionAnimation()->goal();
        const auto s = w->sizeAnimation()->goal();
        t.natural = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                          std::max(1.0, s.x), std::max(1.0, s.y)};
        // When several windows share one geometry — maximized windows, off-screen
        // workspaces — the unfocused cards land under the focused one (drawn last), so
        // their exit is invisible whatever it animates to. Each gets its own exit.
        if (carouselMode() && i != m_scene.focused() && selNat.w >= 1.0 &&
            std::abs(t.natural.x - selNat.x) < 1.0 &&
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
  restorePreviewVisibility();
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
  m_pendingOpen = false;
  m_pendingTicks = 0;
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
  restorePreviewVisibility();
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
  // m_active is down, so the hooks pass focus through again; this is what actually
  // gives the desktop its focus back rather than waiting for the next mouse move.
  dropFocusFromWindows();
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
    // Disarm, never reset. This runs from inside the timer OWN callback, while the event loop
    // iterates its timer list: dropping the last shared pointer here destroys the timer from
    // inside CEventLoopManager::nudgeTimers(), which then walks freed memory. Backtrace of the
    // Hyprland SIGSEGV that killed the compositor on reload:
    //   CSharedPointer<CEventLoopTimer>::_delete -> nudgeTimers -> wl_event_loop_dispatch_idle
    // The object lives as long as the Overview; it is re-armed below when work resumes.
    if (m_animPump)
      m_animPump->cancel();
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
    if (m_pixel.snapshot(w))
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

    m_pixel.putSnapshot(w, fb->getTexture());
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
