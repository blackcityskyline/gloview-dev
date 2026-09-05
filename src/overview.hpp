#pragma once

#include <chrono>
#include <cstdint>
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

#include "anim/clocks.hpp"
#include "model/model.hpp"
#include "render/backdrop.hpp"
#include "blur.hpp"
#include "cursor.hpp"
#include "layout.hpp"

class CFunctionHook;
class CEventLoopTimer;

namespace gloview {

inline model::SwapStyle parseSwapStyle(const std::string &s) {
  return s == "slidevert"  ? model::SwapStyle::SlideVert
       : s == "fade"      ? model::SwapStyle::Fade
       : s == "pop"       ? model::SwapStyle::Pop
                          : model::SwapStyle::Horizontal;
}

// Animation curves are resolved through the registry (anim/curves.hpp):
// leaves carry a curve NAME from the config, native built-ins and
// Lua-registered functions (hl.plugin.gloview.curve) live in one namespace.

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
// a blurred backdrop (window content drawn immediately by the painter); real windows
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

  // wired to Hyprland's event bus / render pass: renderStage is the frame
  // entry (BUILD), paint is the painter it schedules (EXECUTION).
  void renderStage(eRenderStage stage);
  // The painter — one call stack, fixed z-slots (see render/painter.cpp).
  // Reads Model/Clocks/Pixels, mutates nothing.
  void paint();
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
  void renderStripWindows() const; // live window surfaces inside the strip cards
  void
  renderStripButtons() const;  // per-card close-all button + drag destination
                               // hint, drawn after the strip's live surfaces
  void renderPulses(bool strip) const; // swap success rings (grid|strip)
  void renderPreviews() const; // static tiles' chrome (shadow/border/backing),
                                // drawn under the strip
  void renderMainWindows() const; // live window surfaces for the main-area tiles
  void renderGhosts() const; // removed-tile fade-out (populate mirror), drawn
                             // UNDER the mains
  void renderTileButtons()
      const; // per-window "✕", drawn after the tiles' live surfaces
  void
  renderDragTile() const; // the picked-up tile's chrome, drawn over the strip
  void renderDragWindow() const; // the picked-up tile's live surface
  void renderCursorOnTop()
      const; // hardware or software cursor over our overlay (sees HW/SW split)
  // Re-arm the animation loop. Build-time damage+scheduleFrame alone raced
  // with Hyprland's per-frame damage snapshot and still produced 2-3 partial
  // frames; the authoritative re-arm is an EVENT-LOOP timer (rearmanim's
  // pump) that ticks strictly BETWEEN frames, where a fresh full-monitor
  // damage cannot be consumed by an in-flight commit.
  void rearmanim() const;
  void ensureAnimPump();               // (re)arm the between-frames ticker
  SP<CEventLoopTimer> m_animPump;      // null when no animation is running
  // Close-completion teardown, run from the painter's EXECUTION tail (after
  // the cursor): the immediate route reads m_tiles/m_strip while the pass
  // draws, so the Model must stay intact until everything has painted. Still
  // strictly before the next frame's shouldRenderWindow decision — the
  // handoff semantics (real windows reappear on the frame AFTER the final
  // overlay frame) are unchanged.
  void finishPendingDeactivate();
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
  // Entry-animation window only (bounded ~200-400ms) — used to gate the
  // shouldRenderWindow diagnostic trace to just the open() transition instead
  // of the whole time the overview happens to stay up. See black-blink notes
  // in CANDIDATES.md.
  [[nodiscard]] bool opening() const { return m_opening; }
  // Diagnostic-only: monotonic count of RENDER_LAST_MOMENT calls for our
  // tracked monitor, ticked in renderStage() regardless of m_active — lets
  // hkShouldRenderWindow (session.cpp, a different TU) stamp each hide/show
  // decision with the same frame index the "F t=+" trace uses, so the two
  // logs can be correlated frame-for-frame after the fact. Multi-monitor:
  // over-counts by one tick per extra monitor and before the FIRST open()
  // this session (m_monitor unset) — acceptable for the single-monitor
  // black-blink repro this exists for.
  [[nodiscard]] std::uint64_t frameTick() const { return m_frameTick; }
  [[nodiscard]] PHLMONITOR monitor() const { return m_monitor.lock(); }
  [[nodiscard]] bool
  blurEnabled() const; // plugin:gloview:blur != 0 (queried by the pass)
  // The cached FULLSCREEN blurred-wallpaper texture (or null). Read by the
  // tile chrome to frost translucent previews during the entry fade — see
  // drawPreviewChrome().
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
  HANDLE m_handle = nullptr;
  bool m_active = false;
  bool m_opening = false;
  std::chrono::steady_clock::time_point m_openStamp{}; // frame-trace t=0
  std::uint64_t m_frameTick = 0; // diagnostic-only, see frameTick() above

  // Close animation just hit progress 0 THIS frame. shouldRenderWindow (which
  // hides the real windows) is evaluated early in the frame, before our
  // RENDER_LAST_MOMENT pass; if we flipped m_active off mid-frame we'd skip
  // drawing the overlay on a frame whose real windows were already suppressed →
  // one fully-transparent frame (the close-flicker). Instead we draw this final
  // frame's overlay (opaque previews at natural pos cover the windows), then
  // deactivate AFTER the pass is built, so the NEXT frame's early window
  // decision sees m_active=false and renders the real windows cleanly.
  bool m_pendingDeactivate = false;
  bool m_pendingJumpClose  = false; // set after jump ws-slide; fires close() when slide settles
  double m_progress = 0.0;
  anim::Tween m_timeline; // master open/close clock (direction via m_opening);
                    // drives the CHROME only (backdrop/strip/buttons)
  mutable std::chrono::steady_clock::time_point m_lastAnimTick{};
  // Tiles ride their OWN clock, never m_progress: any layout change (open,
  // drop reflow, sync, desktop flip, close) is just "retarget + restart".
  // natural = where the tile is shown now, target = where it must land.
  anim::Tween m_tileClock;
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
  std::vector<model::Tile> m_tiles;
  std::vector<model::StripItem> m_strip;
  // Text->texture cache for tile/strip labels — see model::LabelTex.
  std::unordered_map<void *, model::LabelTex> m_labelCache;
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
  anim::Tween m_newCard; // "+" card pop-in clock (easeOutBack in newCardScale)
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
  anim::Tween m_stripTween;
  void animateStripTo(double from, double to);
  // One rebuild-transition clock: entries (Tile.appear) and ghost exits both
  // ride it, each side in its own leaf's window.
  anim::Tween m_rebuildClock;
  std::vector<model::Ghost> m_ghosts;
  // Per-frame resolved animation state, refreshed at the top of
  // updateAnimation; paint reads these and never resolves config itself
  // (leaf() allocates its curve string — fine once per frame, not per tile).
  anim::AnimCfg m_glide, m_entry, m_ghost, m_lift;
  std::string m_enterStyle, m_exitStyle;
  // True while ANY animation can still produce motion (master pump predicate).
  bool animBusy() const;
  // True while ANY secondary clock is mid-flight (entries/ghosts, strip
  // scroll). The animation-pump predicates MUST include this: after a card/
  // digit workspace switch the master timeline sits pinned at 1 and tiles'
  // clock is done — without these terms the pump never arms, the following
  // frames commit with EMPTY damage over stale buffers, and the transition
  // visibly shakes.
  bool secondaryAnimsActive() const {
    return !m_rebuildClock.done(leaf(entryLeaf()).ms) ||
           !m_rebuildClock.done(leaf(ghostLeaf()).ms) ||
           !m_stripTween.done(leaf("strip_step").ms);
  }
  double tileAppear(int i) const; // staggered 0..1 for tile i
  // Leaf-name selectors for the rebuild choreography (Clocks domain,
  // implemented in anim/clocks.cpp). Precedence everywhere: the expo flip
  // (expo_in/expo_out) > a workspace switch (ws_in/ws_out) > plain populate.
  const char *entryLeaf() const; // newcomer tiles' group
  const char *ghostLeaf() const; // outgoing ghosts' group
  const char *glideLeaf() const; // tile-glide family: reflow | expo halves
  // Content/chrome alpha multiplier for a populating tile: 1 everywhere
  // except the ws_enter_anim == "fade" entry, where it ramps with appear.
  double entryFade(size_t i) const;
  void kickPulse(const PHLWINDOW &w);
  // Swap/drop transitions (anim leaves "drop"/"swap_main"/"swap_partner"):
  // beginSwapFX flies a window that now renders as a STRIP thumb from `from`
  // into its slot; landAfterMove dispatches per landing surface (strip ->
  // flight, grid -> tile glide from oldBox). Called from the drop/swap
  // handlers after the rebuild; ms/curve come from the caller's leaf.
  void beginSwapFX(const PHLWINDOW &w, const LRect &from,
                   model::SwapStyle style, double ms,
                   const std::string &curve);
  void landAfterMove(const PHLWINDOW &w, const LRect &oldBox, double ms,
                     const std::string &curve);
  [[nodiscard]] bool swapfxActive(const PHLWINDOW &w) const;
  void renderSwapFX() const; // Z2.5: flying windows, above the strip

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
  model::Drag m_drag;
  // The drag visual's content box at RELEASE (logical) — the `from` for
  // landing animations, consumed by the drop/swap handlers after m_drag
  // is reset.
  LRect m_lastDragBox;


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
  mutable render::BlurCache m_blur;
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
  void updateSnapshots(); // snapshot mode: refresh m_snapshots from current
                          // tile windows' last committed textures
  // plugin:gloview:cursor_mode == "software" forces a software cursor
  // (lockSoftware + erase+redraw on every frame). Default "auto" lets Hyprland
  // use its hardware cursor plane when the driver supports one — zero GPU cost
  // per move, zero framebuffer pollution, zero trails.
  std::string cursorMode() const;

  // ---- animation registry -------------------------------------------------
  // Resolve one animation group to {on, ms >= 1, curve} — the single choke
  // point for the <leaf>_enabled/_ms/_curve contract (see anim/clocks.cpp,
  // config.cpp for the schema, anim/curves.hpp for curves).
  anim::AnimCfg leaf(const char *name) const;

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
  // Tile-glide window (entry, reflow, close-home all ride one clock). During
  // an expo flip the glide IS the spread/collapse — it reads the expo halves.
  double glideDur() const { return m_tileClockMs > 0 ? m_tileClockMs : leaf(glideLeaf()).ms; }
  // "+" card pop-in duration: never shorter than the tile glide it overlaps.
  double newCardDur() const { return std::max(120.0, leaf("new_card").ms); }
  double tileProgress(int i) const; // staggered raw progress for tile i
  LRect
  currentBox(const model::Tile &t,
             int i) const; // lerped natural->target, staggered + overshoot
  LRect
  tileContentBox(size_t i,
                 const LRect &slot) const; // slot fitted to the window's aspect
  LRect dragBox() const; // the picked-up tile's TARGET box at the cursor
  LRect dragStripBox() const;    // the picked-up strip thumb's TARGET box
  double dragLiftProgress() const;  // 0..1 lift animation progress
  LRect dragVisualBox() const;  // where the preview IS: pickup flight or target
  int draggedTile()
      const; // m_drag.idx while a grid drag is lifted (bounds-checked), else -1
  void
  drawPreviewTile(size_t i, const LRect &slot,
                  bool lift) const; // tile chrome (shadow/border/backing/title)
  void drawPreviewChrome(size_t i, const LRect &lb,
                         bool lift) const; // same, box already content-fitted
  // instant=true: collapse to the target workspace immediately without playing
  // the ws-slide animation — used by jump mode so close() starts from a clean
  // settled state rather than colliding with a mid-flight slide.
  void switchToWorkspace(const model::StripItem &it, bool instant = false);
  void dropOnWorkspace(const PHLWINDOW &w, const model::StripItem &it);
  // Shared tail of both drag-release paths (grid tile / strip window): if the
  // point sits on a workspace card other than skipItem, move w there — or swap
  // it with that card's last-focused window on an RMB drop. True if consumed.
  bool dropOnStripCard(const PHLWINDOW &w, double lx, double ly, int skipItem);
  // Find the strip card that owns workspace w->m_workspace. Returns nullptr if
  // not found (e.g. scratchpad, workspace not in strip).
  model::StripItem *homeStripCardFor(const PHLWINDOW &w);
  void swapOnWorkspace(
      const PHLWINDOW &w,
      const model::StripItem &it); // RMB-drop-on-card counterpart to dropOnWorkspace:
                            // swaps w with the target workspace's last-focused
                            // window instead of moving it
  void swapTiles(int a, int b); // drag a preview onto another → swap the two
                                // windows' places (real layout + overview)
  bool swapWindows(const PHLWINDOW &a,
                   const PHLWINDOW &b); // real-slot swap core (grid+strip)
  std::vector<model::WinPulse> m_pulses;
  // Drag/swap landings: windows flying from their release point (the drag
  // preview under the cursor, or the old slot on a swap) into the new slot.
  // Strip thumbnails have no glide machinery of their own — this is their
  // motion; grid tiles fly via natural->target and skip landings.
  std::vector<model::SwapFX> m_swapfx;
  // Workspace-switch slide: the direction (sign of the new-vs-old ws id) of
  // the CURRENT ws-switch transition. 0 = no slide (transitions end, or a
  // drop/swap rebuild). Newcomer tiles slide in from that side, the removed
  // ones slide out to the opposite side.
  int m_wsSlideDir = 0;
  // The all<->one expo flip riding the same populate clock as a ws switch:
  // +1 = one->all spreading (expo_in leaf), -1 = all->one collapsing
  // (expo_out). Set by toggleAllWorkspaces, cleared with m_wsSlideDir when
  // both transition windows finish. Takes precedence over m_wsSlideDir in
  // every leaf selector — an expo flip is not a ws switch (no directional
  // styles, its own timing).
  int m_expoFlip = 0;
  bool m_jumpMode = false;           // set by instant switchToWorkspace; drives jump_in/out leaves
  double m_tileClockMs = 0;       // snapshotted glide duration at startTileGlide()
  std::string m_tileClockCurve;   // snapshotted glide curve  at startTileGlide()
  // The lift clock: the drag preview's appearance ramp (drag_lift leaf).
  anim::Tween m_dragLiftClock;
  void drawPulseRing(const CBox &boxPx, int round, float roundPow,
                     const CHyprColor &col, double p) const;
  void addWorkspace();          // "+" card: create a workspace (animate it in,
                                // optionally follow)
  void closeWorkspaceWindows(
      const model::StripItem
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
  stripWinSlotRect(const model::StripItem &it, const LRect &card,
                   size_t j) const; // a strip window's on-screen slot rect
  void drawDragStripChrome()
      const; // chrome for a strip-window drag
  // keyboard navigation
  bool keyMatches(int keycode, uint32_t mods, const std::string &combo)
      const; // keycode+held mods ∈ the combo list (names or "shift+tab"
             // combos; empty = disabled)
  int keyIndex(int keycode, uint32_t mods,
               const std::string &combo) const; // 0-based position of keycode
                                                // in the list, else -1
                                                // (number-row → strip card N)
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
