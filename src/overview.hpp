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

  void begin() { start = clock::now(); }
  // Re-anchor at the CURRENT raw value, discarding any wall-time that passed
  // while the compositor was not producing frames (damage-chain stalls, VFR,
  // system hiccups). Without this, a multi-frame render hole silently
  // fast-forwards the whole animation — the open transition appeared to
  // "skip to the end" whenever the frame chain broke right after open().
  void compensateStall(double gapMs, double durMs, double lastRaw) {
    // Re-anchor at the LAST KNOWN pre-gap value. Re-anchoring at the current
    // raw would be a no-op: by now it already includes the hole.
    if (gapMs > 100.0)
      seek(lastRaw, durMs);
  }
  void seek(double frac, double durMs) {
    const auto ms = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double, std::milli>{std::clamp(frac, 0.0, 1.0) *
                                                  std::max(1.0, durMs)});
    start = clock::now() - ms;
  }
  void pinEnd(double durMs) { seek(1.0, durMs); }
  // Linear 0..1, clamped.
  [[nodiscard]] double raw(double durMs) const {
    return std::clamp(
        std::chrono::duration<double, std::milli>(clock::now() - start)
                .count() /
            std::max(1.0, durMs),
        0.0, 1.0);
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
    PHLWORKSPACEREF ws;
    int id = 0;
    bool active = false;
    bool isPlus = false;
    bool isAll =
        false; // the leading "All workspaces" card (toggles the expo view)
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
  Tween m_timeline; // master open/close clock (direction via m_opening)
  mutable std::chrono::steady_clock::time_point m_lastAnimTick{};
  // Last-known timeline positions, captured every animated frame — the anchor
  // points stall compensation rewinds to.
  double m_timelineRaw = 0.0;
  double m_reflowRaw = 0.0;
  double m_newCardRaw = 0.0;
  // A post-move reflow glides the tiles into their new slots WITHOUT re-running
  // the chrome (backdrop + strip) reveal. m_progress stays pinned at 1 (chrome
  // settled) while this separate timer drives the tile natural->target lerp, so
  // the strip no longer re-slides and the backdrop no longer flashes on a drop.
  bool m_reflowing = false;
  Tween m_reflow; // post-move tile-glide clock (chrome stays settled at 1)
  PHLMONITORREF m_monitor;
  PHLWORKSPACEREF m_workspace;    // workspace shown in the main area
  PHLWORKSPACEREF m_liveWsAtOpen; // monitor's live active workspace when opened
                                  // (exit_on_switch)
  std::vector<Tile> m_tiles;
  std::vector<StripItem> m_strip;
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
  // Canvas mode is purely VISUAL: dragging a preview parks it here (window* →
  // canvas box, monitor-local) so the arrangement survives per-frame rebuilds.
  // Dragging a preview never floats/moves the real window — that stays put.
  std::unordered_map<void *, LRect> m_canvasPos;
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
  double m_stripScrollMax = 0.0; // max scroll (0 when the cards fit the band)

  // plugin:gloview:close_trigger == "doubleclick": a plain click on a tile
  // normally activates it IMMEDIATELY (focusAndClose), which leaves no room to
  // detect a second click on the same tile afterward — the overview is simply
  // gone by then. So in this mode the single click is deferred behind a short
  // timer instead: a genuine second click on the SAME window before it fires
  // cancels the deferred activate and closes the window instead (keeping the
  // overview open), the same single-select-vs-double-open pattern any desktop
  // file manager uses. See onMouseButton / cancelPendingClick().
  PHLWINDOWREF m_lastClickWin; // window from the most recent tile click,
                               // whatever the outcome
  PHLWINDOWREF
      m_pendingClickWin; // window the deferred single click would focus+close
  std::chrono::steady_clock::time_point m_lastClickTime;
  SP<CEventLoopTimer> m_clickTimer;

  // drag-and-drop of a window preview onto a workspace card
  int m_pressTile = -1;              // tile under the press (drag candidate)
  bool m_dragging = false;           // moved past the threshold
  double m_pressX = 0, m_pressY = 0; // monitor-local press point
  double m_grabDX = 0, m_grabDY = 0; // cursor offset inside the tile at grab
  double m_dragX = 0, m_dragY = 0;   // current monitor-local cursor
  // Which button armed the current drag (BTN_LEFT/BTN_RIGHT) — dropping a grid
  // tile or a strip window onto a DIFFERENT workspace card moves it with the
  // left button, swaps it with that workspace's window with the right button
  // (task #8).
  int m_pressButton = 0;

  // drag-and-drop of a window picked up directly FROM A STRIP CARD (not the
  // main grid). Indices rather than a pointer since m_strip is rebuilt on any
  // change.
  int m_pressStripItem = -1;   // index into m_strip
  int m_pressStripWin = -1;    // index into m_strip[m_pressStripItem].wins
  PHLWINDOWREF m_dragStripWin; // resolved window being dragged from the strip

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
  // "+" card pop-in duration: never shorter than the tile glide it overlaps.
  double newCardDur() const { return std::max(120.0, animDuration()); }
  double tileBaseProgress()
      const; // 0..1 driver for tile glide (reflow timer or m_progress)
  double tileProgress(int i) const; // staggered raw progress for tile i
  LRect
  currentBox(const Tile &t,
             int i) const; // lerped natural->target, staggered + overshoot
  LRect
  tileContentBox(size_t i,
                 const LRect &slot) const; // slot fitted to the window's aspect
  LRect dragBox() const; // the picked-up tile's box at the cursor
  int draggedTile()
      const; // m_pressTile while a grid drag is live (bounds-checked), else -1
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
  LRect stripCloseButtonRect(
      const LRect &card) const; // "✕" hit/draw rect for a strip card (closes
                                // every window on that workspace)
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
