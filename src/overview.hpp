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
class CGradientValueData; // general:col.active_border's real type
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

// Mission Control-style overview: a workspace strip over a main area of live
// window previews, drawn compositor-side while the real windows are hidden.
class Overview {
public:
  explicit Overview(HANDLE handle);
  ~Overview();

  bool initialize();

  void toggle();

  void open();
  void close();
  void hardClose();           // animation-free teardown for the UNLOAD path
  void toggleDesktop();       // open, or switch into free-arrange mode
  void toggleAllWorkspaces(); // open, or toggle the all-workspaces view
  void altTabInvoke(bool reverse); // opens+seeds if closed, else advances

  // renderStage is the frame entry (BUILD); paint is the painter it schedules
  // (EXECUTION) and mutates nothing.
  void renderStage(eRenderStage stage);
  void paint();

  void renderBackdrop() const;
  // `live` = playing video: re-blur every frame, no cache.
  SP<Render::ITexture> backdropSource(bool &live) const;
  SP<Render::ITexture> renderBackdropSource(int W, int H) const;

  void renderStrip() const;
  void renderStripWindows() const;
  void renderStripButtons() const;
  void renderPulses(bool strip) const;
  void renderPreviews() const;
  // Carousel draws its run in ONE pass: cards overlap, so a card's content must
  // land between its own frame and the next card's, which the grid's split Z1
  // stages cannot express. Replaces those stages, never the strip.
  void renderCarousel() const;
  bool carouselMode() const;
  // The metrics layoutTiles actually used: hit-testing must use the SAME box
  // the renderer drew.
  CarouselMetrics carouselMetrics() const { return m_carouselMetrics; }
  void stepCarousel(int dir); // center the next/previous card, circular
  CarouselLayout m_carousel;     // written by layoutTiles
  CarouselMetrics m_carouselMetrics;
  // The selection m_carousel was built for. Slots are laid out AROUND the
  // selection, so any change needs a relayout; renderStage checks this once, so
  // every existing selection path works here unchanged.
  int m_carouselFor = -1;
  void relayoutCarousel();
  void renderMainWindows() const;
  void renderGhosts() const;
  void renderTileButtons() const;
  void renderDragTile() const;
  void renderDragWindow() const;
  void renderCursorOnTop() const;

  // An EVENT-LOOP timer, not build-time damage+scheduleFrame: the latter raced
  // Hyprland's damage snapshot and produced partial frames, while a timer ticks
  // BETWEEN frames where a fresh full-monitor damage cannot be consumed by an
  // in-flight commit.
  void rearmanim() const;
  void ensureAnimPump();
  SP<CEventLoopTimer> m_animPump; // null when no animation is running

  // Runs from the painter's tail: the immediate route reads m_tiles/m_strip
  // while the pass draws, so the Model must survive until everything painted.
  void finishPendingDeactivate();

  bool isAboveLayer(const std::string &ns) const;
  void renderAboveLayers() const;

  bool onMouseButton(const IPointer::SButtonEvent &e);
  bool onMouseAxis(const IPointer::SAxisEvent &e);
  void onMouseMove();
  void updateHover();
  void onKey(const IKeyboard::SKeyEvent &e, bool &cancel);
  bool shouldHideWindow(const PHLWINDOW &w, const PHLMONITOR &m) const;
  [[nodiscard]] bool isTileWindow(const PHLWINDOW &w) const;

  [[nodiscard]] bool active() const { return m_active; }
  [[nodiscard]] bool opening() const { return m_opening; } // entry anim only
  // Monotonic RENDER_LAST_MOMENT count, ticked whether or not m_active: lets
  // the frame-correlated debug traces line up.
  [[nodiscard]] std::uint64_t frameTick() const { return m_frameTick; }
  [[nodiscard]] PHLMONITOR monitor() const { return m_monitor.lock(); }
  [[nodiscard]] bool blurEnabled() const;
  [[nodiscard]] SP<Render::ITexture> backdropBlurTexture() const {
    return (m_blur.fb && m_blur.fb->isAllocated()) ? m_blur.fb->getTexture()
                                                   : nullptr;
  }
  [[nodiscard]] const std::unordered_map<void *, SP<Render::ITexture>> &
  snapshots() const {
    return m_snapshots;
  }
  [[nodiscard]] bool snapshotMode() const;

private:
  HANDLE m_handle = nullptr;
  bool m_active = false;
  bool m_opening = false;
  std::chrono::steady_clock::time_point m_openStamp{}; // frame-trace t=0
  std::uint64_t m_frameTick = 0;

  // Close anim hit progress 0 THIS frame. shouldRenderWindow runs before our
  // pass, so flipping m_active off mid-frame skips the overlay on a frame whose
  // real windows were already suppressed: one transparent frame.
  bool m_pendingDeactivate = false;
  bool m_pendingJumpClose  = false; // set after a jump ws-slide
  double m_progress = 0.0;
  anim::Tween m_timeline; // master open/close clock; CHROME only
  mutable std::chrono::steady_clock::time_point m_lastAnimTick{};
  // Tiles ride their own clock: any layout change is "retarget + restart".
  anim::Tween m_tileClock;
  // Freeze every tile at the box it is shown at NOW (oldBoxes, captured before a
  // rebuild) and restart. Empty oldBoxes = start from the fresh naturals.
  void startTileGlide(
      const std::vector<std::pair<PHLWINDOW, LRect>> &oldBoxes);
  PHLMONITORREF m_monitor;
  PHLWORKSPACEREF m_workspace;
  PHLWORKSPACEREF m_liveWsAtOpen; // for exit_on_switch
  std::vector<model::Tile> m_tiles;
  std::vector<model::StripItem> m_strip;
  // Text->texture cache by window/workspace pointer. mutable: cachedLabel() is
  // const-callable and caching is its whole job.
  mutable std::unordered_map<void *, model::LabelTex> m_labelCache;
  SP<Render::ITexture> cachedLabel(void *key, const std::string &text,
                                   const CHyprColor &col, int size) const;
  std::vector<std::pair<PHLLSREF, float>> m_hiddenLayers; // + pre-hide alpha
  int m_hovered = -1;      // index into m_tiles
  int m_hoveredStrip = -1; // index into m_strip
  int m_selected = -1;     // keyboard-nav cursor into m_tiles
  // Committed focus target: a workspace change re-asserts focus from its own
  // history, which can still name the previously active window.
  PHLWINDOWREF m_pendingFocus;

  // free-arrange mode: tiles at the windows' real positions, drag floats the
  // real window instead of snapping to a grid.
  bool m_desktopMode = false;
  int m_allOverride = -1; // -1 follow config, 0 off, 1 on
  double m_desktopS = 1.0;  // monitor->preview scale (inverse for drops)
  double m_desktopOx = 0.0; // monitor-local preview origin x
  double m_desktopOy = 0.0; // monitor-local preview origin y
  std::unordered_map<void *, SP<Render::ITexture>> m_snapshots;
  mutable SP<Render::ITexture> m_closeGlyph;
  mutable std::string m_closeGlyphIcon;

  int m_newCardId = 0; // "+" card animating (0 = none)
  bool m_newCardAnim = false;
  anim::Tween m_newCard;
  // Held until close so a still-empty new workspace is not reaped (a single slot
  // leaked all but the last).
  std::vector<PHLWORKSPACEREF> m_newWorkspaces;
  double m_stripScroll = 0.0;    // animated card-group offset on its main axis
  double m_stripScrollMax = 0.0; // 0 when the cards fit
  double m_stripScrollTarget = 0.0;
  double m_stripScrollFrom = 0.0;
  anim::Tween m_stripTween;
  void animateStripTo(double from, double to);
  anim::Tween m_rebuildClock; // entries and ghost exits share it
  std::vector<model::Ghost> m_ghosts;
  // Per-frame resolved animation state; paint never resolves config itself.
  anim::AnimCfg m_glide, m_entry, m_ghost, m_lift;
  std::string m_enterStyle, m_exitStyle;
  bool animBusy() const;
  // Secondary clocks belong in the pump predicates: after a card/digit switch
  // the master timeline is pinned at 1 and tiles' clock is done, so without
  // these terms the pump never arms and later frames commit with EMPTY damage
  // over stale buffers — the transition visibly shakes.
  bool secondaryAnimsActive() const {
    return !m_rebuildClock.done(leaf(entryLeaf()).ms) ||
           !m_rebuildClock.done(leaf(ghostLeaf()).ms) ||
           !m_stripTween.done(leaf("strip_step").ms);
  }
  double tileAppear(int i) const; // staggered 0..1 for tile i
  // Leaf selectors; precedence everywhere: expo flip > ws switch > populate.
  const char *entryLeaf() const;
  const char *ghostLeaf() const;
  const char *glideLeaf() const;
  double entryFade(size_t i) const;
  void kickPulse(const PHLWINDOW &w);
  void beginSwapFX(const PHLWINDOW &w, const LRect &from,
                   model::SwapStyle style, double ms,
                   const std::string &curve);
  void landAfterMove(const PHLWINDOW &w, const LRect &oldBox, double ms,
                     const std::string &curve);
  [[nodiscard]] bool swapfxActive(const PHLWINDOW &w) const;
  void renderSwapFX() const;

  // close_trigger == "doubleclick": the single click is deferred behind a timer.
  // An armed timer with m_pendingClickWin == w IS the "first click just
  // happened" record; a second click before it fires closes the window instead.
  PHLWINDOWREF m_pendingClickWin;
  SP<CEventLoopTimer> m_clickTimer;

  model::Drag m_drag; // pointer PRESS..RELEASE, both drag kinds
  LRect m_lastDragBox; // drag box at RELEASE, the `from` for landings
  bool m_altTabbing = false;

  CHyprSignalListener m_renderStageL;
  CHyprSignalListener m_mouseButtonL;
  CHyprSignalListener m_mouseAxisL;
  CHyprSignalListener m_mouseMoveL;
  CHyprSignalListener m_keyL;
  CFunctionHook *m_shouldRenderHook = nullptr;
  CFunctionHook *m_damageSurfaceHook = nullptr;

  CCursorModule m_cursor; // KMS plane preferred, software erases-and-redraws

  // Backdrop cache: frame one's blur result lives in a persistent FBO, later
  // frames blit it (the background cannot change while up). srcId + recipe are
  // the key, compared EVERY frame, which carries the cache across workspace
  // switches but catches genuine source/recipe changes. A live video source
  // skips the cache by design.
  mutable render::BlurCache m_blur;
  // Full-res FBO holding a rendered desktop background when there is no direct
  // texture to blur. Drawn once per session so a wallpaper engine that
  // screen-captures the desktop cannot pull window content into the re-blur.
  mutable SP<Render::IFramebuffer> m_backdropSrcFB;
  mutable bool m_backdropDrawn = false;
  mutable std::chrono::steady_clock::time_point m_lastBackdropCapture{};
  mutable CBlurFilter m_blurFilter; // independent of decoration:blur:*
  void updateSnapshots();
  std::string cursorMode() const;

  // Single choke point for the <leaf>_enabled/_ms/_curve contract.
  anim::AnimCfg leaf(const char *name) const;

  enum class Anchor { Top, Bottom, Left, Right };
  Anchor stripAnchor() const; // falls back to bar_position
  bool stripHorizontal() const;
  double stripThickness() const; // perpendicular to the anchored edge
  double stripOffset() const;    // inset from it (0 default)
  LRect stripBand() const;             // monitor-local logical
  Vector2D stripSlide(double e) const; // reveal slide-in at progress e
  Vector2D stripScroll() const;
  LRect stripCardAt(size_t i) const;   // card shifted by the scroll
  int stripItemAt(double lx, double ly) const;
  int tileAt(double lx, double ly) const;
  bool showAllWorkspaces() const;
  bool tileBelongs(const PHLWINDOW &w, const PHLMONITOR &m,
                   const PHLWORKSPACE &ws) const; // buildTiles + syncTiles agree
  void buildTiles();
  void buildStrip();
  void layoutTiles();
  void updateAnimation();
  void deactivate();
  // Boundary policy (user-specified): real windows hard-switch at BOTH
  // transition edges — hidden on the first open frame, restored only after the
  // close animation completes. NO per-window fades: during the glide the window
  // resizes and moves, so a fading original under the moving preview reads as
  // double vision. The fade belongs to the BLUR.
  double eased() const; // chrome opacity / backdrop progress
  double animDuration() const; // read LIVE, so config applies in-flight
  double glideDur() const { return m_tileClockMs > 0 ? m_tileClockMs : leaf(glideLeaf()).ms; }
  double newCardDur() const { return std::max(120.0, leaf("new_card").ms); }
  double tileProgress(int i) const;
  LRect currentBox(const model::Tile &t, int i) const;
  LRect tileContentBox(size_t i, const LRect &slot) const;
  LRect dragBox() const;
  LRect dragStripBox() const;
  double dragLiftProgress() const;
  LRect dragVisualBox() const;
  int draggedTile() const;
  void drawPreviewTile(size_t i, const LRect &slot, bool lift) const;
  void drawPreviewChrome(size_t i, const LRect &lb, bool lift) const;
  // instant=true: collapse to the target workspace with no ws-slide, so close()
  // starts from a clean settled state.
  void switchToWorkspace(const model::StripItem &it, bool instant = false);
  void dropOnWorkspace(const PHLWINDOW &w, const model::StripItem &it);
  // Shared tail of both drag-release paths: a point on another card moves w
  // there, or swaps with that card's last-focused window (RMB). True if consumed.
  bool dropOnStripCard(const PHLWINDOW &w, double lx, double ly, int skipItem);
  model::StripItem *homeStripCardFor(const PHLWINDOW &w);
  void swapOnWorkspace(const PHLWINDOW &w, const model::StripItem &it);
  void swapTiles(int a, int b);
  bool swapWindows(const PHLWINDOW &a, const PHLWINDOW &b);
  std::vector<model::WinPulse> m_pulses;
  std::vector<model::SwapFX> m_swapfx; // strip thumbnails' flight machinery
  int m_wsSlideDir = 0;                // sign of new-vs-old ws id, 0 = none
  int m_expoFlip = 0;                  // +1 spreading, -1 collapsing
  bool m_jumpMode = false;             // drives jump_in/out leaves
  double m_tileClockMs = 0;            // snapshotted at startTileGlide
  std::string m_tileClockCurve;
  anim::Tween m_dragLiftClock;
  void drawPulseRing(const CBox &boxPx, int round, float roundPow,
                     const CHyprColor &col, double p) const;
  void addWorkspace();
  void closeWorkspaceWindows(const model::StripItem &it);
  void setDesktopMode(bool on); // flip grid<->canvas, purely visual
  LRect closeButtonRect(const LRect &tile) const;
  bool closeButtonsAlwaysOn() const;
  bool closeOnDoubleClick() const;
  void cancelPendingClick();
  double newCardScale() const;
  float blurStrength() const;
  int blurPasses() const;
  int blurSize() const;
  int blurResolution() const;
  LRect stripWinSlotRect(const model::StripItem &it, const LRect &card,
                   size_t j) const;
  void drawDragStripChrome() const;
  bool keyMatches(int keycode, uint32_t mods, const std::string &combo) const;
  int keyIndex(int keycode, uint32_t mods, const std::string &combo) const;
  void moveSelection(int dx, int dy);
  void activateSelection();
  // A target not on the DISPLAYED workspace (expo, cross-workspace click)
  // collapses onto just that workspace and rebuilds BEFORE the close glide, so
  // the glide aims at the real final position instead of a stale grid.
  void focusAndClose(const PHLWINDOW &w, Desktop::eFocusReason reason);
  bool crossesRealWorkspace(const PHLWINDOW &w) const;
  void syncFocus() const;
  void closeTileWindow(int i);
  std::vector<std::pair<PHLWINDOW, LRect>> captureCurrentBoxes(
      PHLWINDOW exclude = {}) const;
  void replayReflow(std::vector<std::pair<PHLWINDOW, LRect>> &oldBoxes);
  void releaseNewWorkspaces();
  void syncTiles();
  void stepWorkspace(int dir);
  void hideLayers();
  void restoreLayers();
  void restoreFill();
  void stepAltTab(int dir);
  void dbg(const std::string &msg) const;
  void damage() const;
};

} // namespace gloview

inline gloview::Overview *g_overview = nullptr;
