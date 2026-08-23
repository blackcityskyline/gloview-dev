#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace Render {
class ITexture;
class IFramebuffer;
} // namespace Render

namespace Config {
class CGradientValueData; // general:col.active_border / inactive_border's real
                          // type
}

#include "blur.hpp"
#include "cursor.hpp"
#include "layout.hpp"

class CFunctionHook;
class CEventLoopTimer;

namespace gloview {

// Easing curves selectable per animation leaf from the config
// (plugin:gloview:<leaf>_curve). Values mirror the CSS-ish names users know.
enum class Curve : int { Linear, EaseOut, EaseInOut, Back };

inline double curveEval(Curve c, double t) {
  t = std::clamp(t, 0.0, 1.0);
  switch (c) {
  case Curve::Linear: return t;
  case Curve::EaseInOut:
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
  case Curve::Back: { // easeOutBack — small overshoot, for pops only
    const double c1 = 1.70158, c3 = c1 + 1.0;
    return 1.0 + c3 * std::pow(t - 1.0, 3.0) + c1 * std::pow(t - 1.0, 2.0);
  }
  case Curve::EaseOut: break;
  }
  const double inv = 1.0 - t;
  return 1.0 - inv * inv * inv; // easeOutCubic — the historical default
}

inline Curve curveFromName(const std::string &s) {
  if (s == "linear") return Curve::Linear;
  if (s == "easeinout") return Curve::EaseInOut;
  if (s == "back") return Curve::Back;
  return Curve::EaseOut; // "easeout" + anything unparsable
}

// Monotonic timeline anchor for the overview's hand-driven animation clocks.
// Durations live at the call sites (the `duration` config must be picked up
// live even mid-animation), the tween only owns WHEN the clock started plus
// the raw 0..1 math — and the two historical idioms that were previously
// spelled as timestamp arithmetic:
//   seek(frac, dur)  — "continue from raw progress frac" (was: back-date the
//                      start timestamp so now-start == frac*dur)
//   pinEnd(dur)      — "chrome settled, ride at 1.0" (was: subtract a full
//                      duration from the start timestamp)
struct Tween {
  std::chrono::steady_clock::time_point start{};
  mutable double last = 0.0; // last value raw() returned — the anchor stall
                             // compensation rewinds to

  // A fresh run starts at 0 AND resets `last`: the stall guard rewinds to
  // `last`, so keeping a previous run's value (typically 1.0 after an idle
  // period) would make the very first post-begin frame snap the clock to its
  // end — the "close lands instantly while the strip is still collapsing" bug.
  void begin() {
    start = clock::now();
    last = 0.0;
  }
  // Discard any wall-time that passed while the compositor was not producing
  // frames (damage-chain stalls, VFR, system hiccups): re-anchor at the LAST
  // KNOWN pre-gap value — re-anchoring at the current raw would be a no-op,
  // by now it already includes the hole. Without this a multi-frame render
  // hole silently fast-forwards the whole animation (the open transition
  // "skipped to the end" whenever the frame chain broke right after open()).
  void compensateStall(double gapMs, double durMs) {
    if (gapMs > 100.0)
      seek(last, durMs);
  }
  void seek(double frac, double durMs) {
    const auto ms = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double, std::milli>{std::clamp(frac, 0.0, 1.0) *
                                                  std::max(1.0, durMs)});
    start = clock::now() - ms;
    last = std::clamp(frac, 0.0, 1.0);
  }
  void pinEnd(double durMs) { seek(1.0, durMs); }
  bool done(double durMs) const { return raw(durMs) >= 1.0; }
  // Linear 0..1, clamped; refreshes `last`.
  double raw(double durMs) const {
    last = std::clamp(
        std::chrono::duration<double, std::milli>(clock::now() - start)
                .count() /
            std::max(1.0, durMs),
        0.0, 1.0);
    return last;
  }

private:
  using clock = std::chrono::steady_clock;
};

// Plugin config values registered with `addConfigValueV2` (main.cpp), kept so
// the cfg* helpers can read them through their V2 `value()` accessor. The
// deprecated `HyprlandAPI::getConfigValue()` path does NOT observe values set
// from a Lua `hl.config{}` config — it returned the registered default, so
// every setting looked like it "did nothing" under a Lua config. Reading the
// IValue directly works for both the legacy/ini and Lua config frontends.
//
// No `colors` map: every plugin:gloview:<color> option is registered as a
// plain STRING (see cfgColor() in overview_core.cpp) holding a hex literal —
// or a palette-resolved hex produced Lua-side from a theme module (hyprbars
// pattern), so the plugin stays decoupled from any scheme engine.
struct ConfigRegistry {
  std::unordered_map<std::string, SP<Config::Values::CIntValue>> ints;
  std::unordered_map<std::string, SP<Config::Values::CStringValue>> strings;
  std::unordered_map<std::string, SP<Config::Values::CFloatValue>> floats;
};
inline ConfigRegistry g_config;

// macOS Mission Control-style overview for Hyprland.
//
//   ┌───────────────────────────────────────────────┐
//   │  [ws1] [ws2] [ws3] ...                    [ + ] │  translucent strip
//   ├───────────────────────────────────────────────┤
//   │     ┌────┐   ┌────────┐                         │
//   │     │win │   │  win   │   live window previews  │  main area
//   │     └────┘   └────────┘                         │
//   └───────────────────────────────────────────────┘
//
// The whole thing is drawn compositor-side from windows' own LIVE surfaces over
// a blurred backdrop (queued CSurfacePassElements, not snapshots); real windows
// are hidden while it is up. Layout math lives in layout.hpp so it can be
// tweaked independently.
class Overview {
public:
  explicit Overview(HANDLE handle);
  ~Overview();

  bool initialize();

  void toggle();

  void open();
  void close();
  void hardClose(); // immediate, animation-free teardown for the UNLOAD path
                    // (hyprctl gloviewunload)
  void toggleDesktop(); // open (or, if already open, switch into) free-arrange
                        // desktop mode
  void toggleAllWorkspaces(); // open (or, if already open, toggle) the
                              // all-workspaces "expo" main view
  void altTabInvoke(
      bool reverse); // gloview:alttab[back] dispatcher: opens+seeds-at-previous
                     // if closed, else advances

  // wired to Hyprland's event bus / render pass
  void renderStage(eRenderStage stage);
  void renderBackdrop() const;
  // The texture the backdrop blur is sourced from (the wallpaper, or — with
  // fullscreen_background=1 — the fullscreen mpv window on the displayed
  // workspace). `live` reports whether that source is a playing video, which
  // must be re-blurred every frame instead of cached.
  SP<Render::ITexture> backdropSource(bool &live) const;
  // Renders the true desktop background (background color, Hyprland's internal
  // wallpaper texture if any, and every BACKGROUND/BOTTOM layer-shell surface —
  // i.e. any wallpaper engine: noctalia, swaybg, swww, hyprpaper, ...) into the
  // private m_backdropSrcFB at W x H and returns its texture, or nullptr if
  // even that is unavailable.
  SP<Render::ITexture> renderBackdropSource(int W, int H) const;
  void renderStrip() const;
  void
  renderStripWindows() const; // live window surfaces inside the strip cards
  void
  renderStripButtons() const;  // per-card close-all button + drag destination
                               // hint, drawn after the strip's live surfaces
  void renderPulses(bool strip) const; // swap success rings (grid|strip)
  void renderPreviews() const; // static tiles' chrome (shadow/border/backing),
                               // drawn under the strip
  void
  renderMainWindows() const; // live window surfaces for the main-area tiles
  void renderTileButtons()
      const; // per-window "✕", drawn after the tiles' live surfaces
  void
  renderDragTile() const; // the picked-up tile's chrome, drawn over the strip
  void renderDragWindow() const; // the picked-up tile's live surface
  void renderCursorOnTop()
      const; // hardware or software cursor over our overlay (sees HW/SW split)
  // Re-arm the animation loop. Front-phase damage+scheduleFrame alone raced
  // with Hyprland's per-frame damage snapshot and still produced 2-3 partial
  // frames; the authoritative re-arm is an EVENT-LOOP timer (rearmanim's
  // pump) that ticks strictly BETWEEN frames, where a fresh full-monitor
  // damage cannot be consumed by an in-flight commit.
  void rearmanim() const;
  void ensureAnimPump();               // (re)arm the between-frames ticker
  SP<CEventLoopTimer> m_animPump;      // null when no animation is running
  bool isAboveLayer(const std::string &ns) const;
  void renderAboveLayers() const; // re-render opted-in TOP/OVERLAY layer
                                  // surfaces on top of the overview
  bool onMouseButton(const IPointer::SButtonEvent &e);
  bool onMouseAxis(const IPointer::SAxisEvent
                       &e); // scroll the workspace strip when it overflows
  void onMouseMove();
  void updateHover(); // recompute hovered tile/card from current cursor pos
  void onKey(const IKeyboard::SKeyEvent &e, bool &cancel);
  bool shouldHideWindow(const PHLWINDOW &w, const PHLMONITOR &m) const;
  [[nodiscard]] bool isTileWindow(
      const PHLWINDOW &w) const; // is w one of our
                                 // tile windows?
                                 // (distinguishes live-damage-worthy
                                 // tile previews from hidden
                                 // non-tile windows in hkDamageSurface)

  [[nodiscard]] bool active() const { return m_active; }
  [[nodiscard]] PHLMONITOR monitor() const { return m_monitor.lock(); }
  [[nodiscard]] bool
  blurEnabled() const; // plugin:gloview:blur != 0 (queried by the pass)
  // The cached FULLSCREEN blurred-wallpaper texture (or null). Read by the
  // tile chrome to frost translucent previews during the entry fade — see
  // drawPreviewTile().
  [[nodiscard]] SP<Render::ITexture> backdropBlurTexture() const {
    return (m_blur.fb && m_blur.fb->isAllocated()) ? m_blur.fb->getTexture()
                                                   : nullptr;
  }
  [[nodiscard]] const std::unordered_map<void *, SP<Render::ITexture>> &
  snapshots() const { // read by renderWindowLive() for the static-texture path
    return m_snapshots;
  }
  [[nodiscard]] bool
  snapshotMode() const; // plugin:gloview:preview_mode == "snapshot" (queried
                        // by renderWindowLive too)

private:
  struct Tile {
    PHLWINDOWREF win;
    LRect natural; // monitor-local logical: real place (goal); animation start
    LRect target;  // monitor-local logical: grid slot
    bool parked = false; // canvas mode: target is user-placed — rebuilds and
                         // syncs must not move it (only the tile's own drag)
    std::string labelText; // what `label` was rendered from (cache key)
    double appear = 1.0;   // 0..1 population progress: 0 = brand-new to the
                           // grid this rebuild (fades/scales in over
                           // populate); ghosts cover the reverse direction
    SP<Render::ITexture> label; // cached window title, shown on hover
  };

  struct StripWin {
    PHLWINDOWREF win;
    LRect
        rel; // 0..1 within the monitor: the window's tiled slot in the card.
             // The card preview renders the window's LIVE surface into this
             // slot (renderStripWindows), so no snapshot/crop state is needed.
  };

  struct StripItem {
    enum class Kind : int { Ws, Plus, All }; // All = leading expo-toggle card
    PHLWORKSPACEREF ws;
    int id = 0;
    bool active = false;
    Kind kind = Kind::Ws;
    // A placeholder card for a numeric workspace ID that has no real
    // PHLWORKSPACE object yet (Hyprland only keeps workspace objects that were
    // actually created/visited, so an empty never-visited workspace simply
    // doesn't exist to iterate — strip_empty_mode "show"/"neighbors" synthesize
    // these so the strip can still display them). `ws` stays unset;
    // clicking/dropping on it creates the real workspace at exactly `id`, same
    // as "+" but at a specific number instead of the lowest free one.
    bool virtualWs = false;
    LRect card; // monitor-local logical
    std::vector<StripWin> wins;
    SP<Render::ITexture> label; // cached rendered workspace name
  };

  HANDLE m_handle = nullptr;
  bool m_active = false;
  bool m_opening = false;
  std::chrono::steady_clock::time_point m_openStamp{}; // frame-trace t=0

  // Close animation just hit progress 0 THIS frame. shouldRenderWindow (which
  // hides the real windows) is evaluated early in the frame, before our
  // RENDER_LAST_MOMENT pass; if we flipped m_active off mid-frame we'd skip
  // drawing the overlay on a frame whose real windows were already suppressed →
  // one fully-transparent frame (the close-flicker). Instead we draw this final
  // frame's overlay (opaque previews at natural pos cover the windows), then
  // deactivate AFTER the pass is built, so the NEXT frame's early window
  // decision sees m_active=false and renders the real windows cleanly.
  bool m_pendingDeactivate = false;
  double m_progress = 0.0;
  Tween m_timeline; // master open/close clock (direction via m_opening);
                    // drives the CHROME only (backdrop/strip/buttons)
  mutable std::chrono::steady_clock::time_point m_lastAnimTick{};
  // Tiles ride their OWN clock, never m_progress: any layout change (open,
  // drop reflow, sync, desktop flip, close) is just "retarget + restart".
  // natural = where the tile is shown now, target = where it must land.
  Tween m_tileClock;
  // Set every tile's natural to the box it is being shown at RIGHT NOW
  // (`oldBoxes` — captured before a rebuild) and restart the clock: the next
  // frames glide them into the fresh targets. Empty oldBoxes = start from the
  // freshly assigned naturals (plain open).
  void startTileGlide(
      const std::vector<std::pair<PHLWINDOW, LRect>> &oldBoxes);
  PHLMONITORREF m_monitor;
  PHLWORKSPACEREF m_workspace;    // workspace shown in the main area
  PHLWORKSPACEREF m_liveWsAtOpen; // monitor's live active workspace when opened
                                  // (exit_on_switch)
  std::vector<Tile> m_tiles;
  std::vector<StripItem> m_strip;
  // Text->texture cache for tile/strip labels. Tiles and StripItems are
  // RECREATED on every rebuild (each drop/swap/sync), so caching on them
  // re-rasterized every label every drop — the drag&drop stutter. Keyed by
  // the owning window/workspace pointer with mark-and-sweep per build pass
  // (explicit lifecycle, per AGENTS).
  struct LabelTex {
    std::string text;
    SP<Render::ITexture> tex;
  };
  std::unordered_map<void *, LabelTex> m_labelCache;
  SP<Render::ITexture> cachedLabel(void *key, const std::string &text,
                                   const CHyprColor &col, int size);
  // layer surfaces (bars/popups) we faded out while up, with their pre-hide
  // alpha goal, so deactivate() restores them exactly — even if config changed
  // meanwhile.
  std::vector<std::pair<PHLLSREF, float>> m_hiddenLayers;
  int m_hovered = -1;      // index into m_tiles
  int m_hoveredStrip = -1; // index into m_strip
  int m_selected = -1;     // keyboard-nav cursor into m_tiles
  // Window explicitly committed via focusAndClose() (Enter / click on a tile,
  // possibly cross-workspace) whose real focus must survive the async
  // close-animation teardown. CMonitor::changeWorkspace()'s own auto-focus —
  // both the one deactivate() may trigger directly and the one chained
  // synchronously inside fullWindowFocus()'s own cross-workspace path —
  // resolves via CWorkspace::getLastFocusedWindow(), which can still land on
  // whatever was ACTUALLY focused there before this session. Re-asserting
  // this window one more time at the very end of deactivate() guarantees the
  // committed choice wins over stale focus history (see its comment there).
  PHLWINDOWREF m_pendingFocus;

  // free-arrange "desktop" mode: tiles sit at the windows' real positions and a
  // drag floats + repositions the real window instead of snapping to a grid.
  bool m_desktopMode = false;
  // expo view: -1 = follow plugin:gloview:show_all_workspaces, 0 =
  // runtime-forced off, 1 = runtime-forced on (the gloview:allworkspaces
  // toggle). Reset to -1 on full close.
  int m_allOverride = -1;
  double m_desktopS = 1.0;  // monitor→preview scale (and its inverse for drops)
  double m_desktopOx = 0.0; // monitor-local preview origin x
  double m_desktopOy = 0.0; // monitor-local preview origin y
  // plugin:gloview:preview_mode == "snapshot": PHLWINDOW* -> the window's main
  // surface texture captured at tile-build time. renderWindowLive() renders
  // this static texture (CSurfacePassElement with data.texture) instead of
  // walking the LIVE surface tree every frame — see renderWindowLive()'s
  // comment for the measured win. Keyed by raw pointer; cleared on teardown.
  std::unordered_map<void *, SP<Render::ITexture>> m_snapshots;
  mutable SP<Render::ITexture>
      m_closeGlyph; // cached "✕" for the desktop-mode close buttons
  mutable std::string m_closeGlyphIcon; // which icon m_closeGlyph currently
                                        // holds (re-render on config change)

  // "+" add-workspace pop-in: the freshly created card scales up from its
  // center.
  int m_newCardId = 0; // workspace id of the animating card (0 = none)
  bool m_newCardAnim = false;
  Tween m_newCard; // "+" card pop-in clock (easeOutBack in newCardScale)
  // freshly created ("+" or the direct number-key jump) workspaces, held
  // persistent until close so none of them get reaped while empty. A single
  // slot here used to leak every workspace but the last one created in a
  // session (each new one silently overwrote the previous held reference, so
  // its persistent flag never got cleared) — a vector holds all of them so
  // deactivate()/hardClose() can release every one.
  std::vector<PHLWORKSPACEREF> m_newWorkspaces;
  double m_stripScroll = 0.0; // strip group scroll offset along its main axis
                                 // (the ANIMATED value — consumers only read this)
  double m_stripScrollMax = 0.0; // max scroll (0 when the cards fit the band)
  // AN5: target-chase for strip scrolling. buildStrip/stepWorkspace set a new
  // TARGET; animateStripScroll() eases m_stripScroll toward it each animated
  // frame (strip_step leaf). Axis-free: the offset is always along the band's
  // main axis.
  double m_stripScrollTarget = 0.0;
  double m_stripScrollFrom = 0.0;
  Tween m_stripTween;
  void animateStripTo(double from, double to);
  // Population (populate leaf): drives Tile.appear for newcomers and the
  // ghost fade-out for tiles removed by a rebuild.
  Tween m_populate;
  struct Ghost {
    PHLWINDOWREF win;
    LRect box; // monitor-local logical, frozen at removal
  };
  std::vector<Ghost> m_ghosts;
  double populateMs() const { return animMs("populate", nullptr, 250); }
  // True while ANY secondary clock is mid-flight (population/ghosts, strip
  // scroll). The animation-pump predicates MUST include this: after a card/
  // digit workspace switch the master timeline sits pinned at 1 and tiles'
  // clock is done — without these terms the pump never arms, the following
  // frames commit with EMPTY damage over stale buffers, and the transition
  // visibly shakes.
  bool secondaryAnimsActive() const {
    return !m_populate.done(populateMs()) ||
           !m_stripTween.done(animMs("strip_step", nullptr, 200));
  }
  double tileAppear(int i) const; // staggered 0..1 for tile i
  void renderGhosts() const;      // removed-tile fade-out (populate mirror)
  void kickPulse(const PHLWINDOW &w);

  // plugin:gloview:close_trigger == "doubleclick": a plain click on a tile
  // normally activates it IMMEDIATELY (focusAndClose), which leaves no room to
  // detect a second click on the same tile afterward — the overview is simply
  // gone by then. So in this mode the single click is deferred behind a short
  // timer: an armed timer with m_pendingClickWin == w IS the "first click
  // recently happened" record; a second click on the same window before it
  // fires cancels it and closes the window instead (staying open). See
  // onMouseButton / cancelPendingClick().
  PHLWINDOWREF m_pendingClickWin; // window the deferred single click would focus+close
  SP<CEventLoopTimer> m_clickTimer;

  // Everything the pointer did between PRESS and RELEASE, for both drag kinds
  // (a grid tile, a window grabbed straight off a strip card) and every press
  // outcome. One value instead of the old eleven scattered members — a stale
  // half-armed drag can no longer exist across sessions (open() just resets
  // it), and release logic switches on `press` instead of decoding sentinels.
  struct Drag {
    enum class Press : int {
      Tile,      // pressed on a main-grid tile: idx = m_tiles index
      StripWin,  // pressed on a strip card's window slot: idx = m_strip index,
                 // winIdx = index into its wins, win = the window
      StripCard, // pressed elsewhere on a strip card — switch already happened
                 // on press; release must do nothing
      Empty,     // empty space → close on release unless consumed
      Consumed,  // press fully handled on the spot (e.g. a ✕ button)
    };
    Press press = Press::Empty;
    int idx = -1, winIdx = -1;
    PHLWINDOWREF win;
    int button = 0;
    double pressX = 0, pressY = 0;   // monitor-local press point
    double grabDX = 0, grabDY = 0;   // cursor offset inside the grabbed box
    double x = 0, y = 0;             // current cursor, kept fresh by updateHover
    bool lifted = false;             // moved past the threshold → a real drag
    bool armed() const { return press == Press::Tile || press == Press::StripWin; }
  };
  Drag m_drag;


  // Alt-Tab session: armed by altTabInvoke() on open, released when the
  // configured modifier goes up (commit-on-release) or the overview closes.
  // The grid itself is NOT reordered — tiles keep their spatial layout and
  // stepAltTab just walks m_selected in tile order. (MRU/smart arrangement was
  // removed; a replacement ranking may be designed later.)
  bool m_altTabbing = false;


  CHyprSignalListener m_renderStageL;
  CHyprSignalListener m_mouseButtonL;
  CHyprSignalListener m_mouseAxisL;
  CHyprSignalListener m_mouseMoveL;
  CHyprSignalListener m_keyL;
  CFunctionHook *m_shouldRenderHook = nullptr;
  CFunctionHook *m_damageSurfaceHook = nullptr;

  // Hardware-accelerated cursor: prefers the KMS cursor plane (zero framebuffer
  // writes, zero trails, zero GPU work per move). Falls back to a software
  // cursor that erases its previous position with an opaque backdrop rect.
  CCursorModule m_cursor;

  // Blurred-backdrop cache: the first frame's backdrop blur result is
  // rendered into a persistent FBO; every subsequent frame blits that texture
  // instead of re-invoking the expensive blur shader (the desktop background
  // doesn't change while the overview is up — all windows are hidden by
  // shouldRenderWindow). valid=false forces a re-blur. srcId is the identity
  // of the texture the fb was blurred from: a direct texture (fullscreen mpv
  // surface or monitor wallpaper) or nullptr for the frozen wallpaper-layers
  // FBO. renderBackdrop() resolves the source EVERY frame and invalidates on
  // id change — that check is what carries the cache across plain workspace
  // switches (same wallpaper ⇒ no re-blur, no brightness shift) while still
  // catching genuine source changes nothing else marks dirty. A live video
  // source skips the cache every frame by design.
  struct BlurCache {
    SP<Render::IFramebuffer> fb;
    const void *srcId = nullptr;
    // The filter recipe the fb was rendered with. Part of the cache KEY
    // (compared every frame together with srcId): without it, changing
    // blur_passes/blur_size/blur_resolution/blur_strength mid-session would
    // keep showing a texture baked with the old params until reopen.
    int passes = 0, sizePx = 0, resolution = 0;
    float strength = -1.0F;
    bool valid = false;
    [[nodiscard]] bool matches(const void *id, int p, int s, int r,
                               float st) const {
      return valid && srcId == id && passes == p && sizePx == s &&
             resolution == r && strength == st;
    }
    void invalidate() { valid = false; }
    void drop() { // full teardown: free the FBO too (open / unload)
      invalidate();
      fb.reset();
    }
  };
  mutable BlurCache m_blur;
  // Full-res FBO holding the freshly-rendered desktop background (wallpaper
  // layers) when there's no direct texture source to blur.  Drawn once per
  // overview session (at open / on layer-surface commits only) so workspace
  // switches re-blur from the same frozen source — a wallpaper engine that
  // screen-captures the composited desktop would otherwise introduce window
  // content (e.g. foot text) into the re-drawn FBO on every re-blur.
  mutable SP<Render::IFramebuffer> m_backdropSrcFB;
  mutable bool m_backdropDrawn = false; // false → redraw layers next time
  // While true, the frozen backdrop source + blur cache are invalidated EVERY
  // frame until the populate clock settles. A one-shot invalidate right after
  // a ws switch raced the wallpaper engine's repaint and re-froze the OLD
  // content for the whole transition.
  bool m_backdropRecapture = false;
  // Self-contained blur (own GL program): plugin-tunable blur_passes /
  // blur_size / blur_resolution, independent of Hyprland's global
  // decoration:blur:* (which plugins can't override per-call).
  mutable CBlurFilter m_blurFilter;

  // config helpers
  int cfgInt(const char *name, int fallback) const;
  float cfgFloat(const char *name, float fallback) const;
  std::string cfgStr(const char *name, const char *fallback) const;
  void updateSnapshots(); // snapshot mode: refresh m_snapshots from current
                          // tile windows' last committed textures
  // plugin:gloview:cursor_mode == "software" forces a software cursor
  // (lockSoftware + erase+redraw on every frame). Default "auto" lets Hyprland
  // use its hardware cursor plane when the driver supports one — zero GPU cost
  // per move, zero framebuffer pollution, zero trails.
  std::string cursorMode() const;
  // Unified color read — see cfgColor() in overview_core.cpp for the value
  // grammar (hex literal, or a palette-resolved hex produced Lua-side).
  Hyprlang::INT cfgColor(const char *base, const char *fallback) const;

  // ---- animation registry (AN1) -----------------------------------------
  // Every animation is a config "leaf": <leaf>_enabled / <leaf>_ms /
  // <leaf>_curve (see the kAnimCfg table in main.cpp), under one master
  // switch animations_enabled that gates EVERYTHING. _ms = -1 means "follow
  // the legacy `duration` option", preserving single-knob configs.
  struct AnimCfg {
    bool on = false;
    int ms = 1;
    Curve curve = Curve::EaseOut;
  };
  AnimCfg anim(const char *leaf) const;
  // Effective duration for a leaf: 1ms when master/leaf disabled (every clock
  // then completes within one frame — the whole plugin goes static without
  // any per-site branching), else <leaf>_ms or its fallback.
  double animMs(const char *leaf, const char *msFallbackKey,
                int msFallback) const;

  // Which monitor edge the workspace strip is anchored to. Top/Bottom give a
  // horizontal strip (cards in a row); Left/Right give a vertical strip (cards
  // in a column).
  enum class Anchor { Top, Bottom, Left, Right };
  Anchor
  stripAnchor() const; // plugin:gloview:anchor (falls back to bar_position)
  bool stripHorizontal() const; // Top or Bottom
  double
  stripThickness() const; // band size perpendicular to its edge (strip_height)
  double
  stripOffset() const; // inset from the anchored edge (strip_offset, 0 default)
  LRect stripBand() const;             // the band rect, monitor-local logical
  Vector2D stripSlide(double e) const; // reveal slide-in offset at progress e
  Vector2D stripScroll() const; // current scroll offset of the card group
  LRect stripCardAt(size_t i)
      const; // m_strip[i].card shifted by the current scroll (for hit-testing)
  int stripItemAt(double lx,
                  double ly) const; // card index under the point, else -1
  int tileAt(double lx,
             double ly) const; // tile whose currentBox holds the point, else -1
  bool showAllWorkspaces()
      const; // effective expo state: runtime override (m_allOverride) else
             // plugin:gloview:show_all_workspaces
  bool tileBelongs(
      const PHLWINDOW &w, const PHLMONITOR &m,
      const PHLWORKSPACE &ws) const; // shared main-area membership test
                                     // (buildTiles + syncTiles MUST agree)
  void buildTiles();
  void buildStrip();
  void layoutTiles();
  void updateAnimation();
  void deactivate();
  // Boundary policy (deliberate, user-specified): real windows hard-switch at
  // BOTH transition edges — hidden on the first open frame, restored only
  // after the close animation completes. NO per-window fades: during the glide
  // the window changes size and position, so a fading original under the
  // moving preview reads as double vision. Seamlessness comes from the previews
  // being pixel-identical to the real windows at natural boxes on both ends.
  // The fade belongs to the BLUR (backdrop crossfade), never to the windows.
  double eased() const; // opacity / backdrop progress
  // plugin:gloview:duration with the shared floor (ms), read LIVE so config
  // changes apply to in-flight animations.
  double animDuration() const;
  // Tile-glide leaf (entry, reflow, close-home all ride one clock).
  double reflowDur() const {
    return animMs("reflow", "plugin:gloview:duration", 360);
  }
  // "+" card pop-in duration: never shorter than the tile glide it overlaps.
  double newCardDur() const {
    return std::max(120.0, animMs("new_card", "plugin:gloview:duration", 360));
  }
  double tileProgress(int i) const; // staggered raw progress for tile i
  LRect
  currentBox(const Tile &t,
             int i) const; // lerped natural->target, staggered + overshoot
  LRect
  tileContentBox(size_t i,
                 const LRect &slot) const; // slot fitted to the window's aspect
  LRect dragBox() const; // the picked-up tile's box at the cursor
  int draggedTile()
      const; // m_drag.idx while a grid drag is lifted (bounds-checked), else -1
  void
  drawPreviewTile(size_t i, const LRect &slot,
                  bool lift) const; // tile chrome (shadow/border/backing/title)
  void switchToWorkspace(const StripItem &it);
  void dropOnWorkspace(const PHLWINDOW &w, const StripItem &it);
  // Shared tail of both drag-release paths (grid tile / strip window): if the
  // point sits on a workspace card other than skipItem, move w there — or swap
  // it with that card's last-focused window on an RMB drop. True if consumed.
  bool dropOnStripCard(const PHLWINDOW &w, double lx, double ly, int skipItem);
  void swapOnWorkspace(
      const PHLWINDOW &w,
      const StripItem &it); // RMB-drop-on-card counterpart to dropOnWorkspace:
                            // swaps w with the target workspace's last-focused
                            // window instead of moving it
  void swapTiles(int a, int b); // drag a preview onto another → swap the two
                                // windows' places (real layout + overview)
  bool swapWindows(const PHLWINDOW &a,
                   const PHLWINDOW &b); // real-slot swap core (grid+strip)
  // Success pop after a swap (anim_swap_pulse): window-keyed so rebuilds
  // between kick and render can't orphan it. Rendered as a ring flash around
  // wherever the window currently sits (grid tile or strip slot).
  struct WinPulse {
    PHLWINDOWREF w;
    double p = 0.0; // accumulated FRAME progress, not wall-clock: heavy
                    // frames (layout recalc right after a drop) must not
                    // leap it across the easeOutBack plateau — that read as
                    // the ring snapping wide and freezing
    std::chrono::steady_clock::time_point last;
  };
  std::vector<WinPulse> m_pulses;
  void drawPulseRing(const CBox &boxPx, int round, float roundPow,
                     const CHyprColor &col, double p) const;
  void addWorkspace();          // "+" card: create a workspace (animate it in,
                                // optionally follow)
  void closeWorkspaceWindows(
      const StripItem
          &it); // middle-click a card: send-close every window on it
  void setDesktopMode(
      bool on); // flip grid<->canvas while open, gliding the previews (purely
                // visual; never mutates a real window)
  LRect closeButtonRect(
      const LRect &tile) const; // "✕" hit/draw rect for a tile content box
                                // (position/size/visibility configurable)
  bool closeButtonsAlwaysOn()
      const; // plugin:gloview:close_button_visibility == "always"
  bool closeOnDoubleClick()
      const; // plugin:gloview:close_trigger == "doubleclick" — replaces the
             // per-window "✕" with a double-click/double-tap on the tile
  void cancelPendingClick(); // drop any pending single-vs-double-click timer
                             // (see m_clickTimer)
  double
  newCardScale() const; // 0..1(+overshoot) pop-in scale for the just-added card
  float blurStrength() const; // plugin:gloview:blur as 0..1 (float); 0 = off
  int blurPasses() const;     // plugin:gloview:blur_passes (gauss iterations)
  int blurSize() const;       // plugin:gloview:blur_size (radius, screen px)
  int blurResolution() const; // plugin:gloview:blur_resolution (1/N downscale)
  // strip-card window drag (picking up and moving a window straight off the
  // strip)
  LRect
  stripWinSlotRect(const StripItem &it, const LRect &card,
                   size_t j) const; // a strip window's on-screen slot rect
  LRect dragStripBox()
      const; // the picked-up strip window's floating box at the cursor
  void drawDragStripChrome()
      const; // chrome for a strip-window drag (shadow/border/backing)
  // keyboard navigation
  bool keyMatches(int keycode, uint32_t mods, const char *cfgName,
                  const char *fallback)
      const; // keycode+held mods ∈ the configured list (names or "shift+tab"
             // combos; empty = disabled)
  int keyIndex(
      int keycode, uint32_t mods, const char *cfgName,
      const char *fallback) const; // 0-based position of keycode in the list,
                                   // else -1 (number-row → strip card N)
  void moveSelection(
      int dx,
      int dy); // step the selection cursor to the nearest tile in a direction
  void activateSelection(); // focus the selected window and dismiss
  // Shared by the mouse-click-to-focus path and activateSelection() (Enter /
  // Alt-Tab commit): focuses `w` and dismisses. If `w` isn't on the currently
  // DISPLAYED workspace (expo view, clicking/committing a window from another
  // workspace), collapses the overview onto just that workspace and rebuilds
  // tiles from it BEFORE starting the close glide, so the close animation
  // targets the window's real final position instead of animating the stale
  // multi-workspace expo grid and only cutting over once it ends (#5).
  void focusAndClose(const PHLWINDOW &w, Desktop::eFocusReason reason);
  // Would focusing `w` right now make Hyprland switch the monitor's REAL
  // active workspace synchronously (and therefore fire its native
  // workspace-switch animation immediately)? See its definition in
  // overview_input.cpp for the source-verified explanation.
  bool crossesRealWorkspace(const PHLWINDOW &w) const;
  void syncFocus() const; // point Hyprland's real focus at the selected tile
                          // (passthrough keybinds)
  void
  closeTileWindow(int i); // send-close a tile's window, then reflow the rest
  std::vector<std::pair<PHLWINDOW, LRect>> captureCurrentBoxes(
      PHLWINDOW exclude =
          {}) const; // every tile's window+currentBox snapshot (reflow tails)
  void replayReflow(std::vector<std::pair<PHLWINDOW, LRect>> &
                        oldBoxes); // glide tiles into new slots after a removal
  void releaseNewWorkspaces(); // clear persistent flags on every "+"-created
                               // workspace held this session
  void syncTiles(); // add/drop tiles when the displayed workspace's window set
                    // changes, then reflow
  void stepWorkspace(int dir); // scroll-wheel over the main area: show
                               // prev/next workspace card
  void hideLayers();    // fade out Top/Overlay layer surfaces (bars) per config
  void restoreLayers(); // restore the alphas hideLayers() saved
  void restoreFill();   // reset m_fillIgnoreSmall on every window (see
                        // renderWindowLive)
  void stepAltTab(int dir); // advance the Alt-Tab selection cursor by dir
                            // (+1/-1) in tile order (circular)
  void
  dbg(const std::string &msg) const; // plugin:gloview:debug_logs gated logging
  void damage() const;
};

} // namespace gloview

inline gloview::Overview *g_overview = nullptr;
