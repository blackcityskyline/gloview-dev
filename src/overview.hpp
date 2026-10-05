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

#include "clock/clocks.hpp"
#include "clock/clock.hpp"
#include "model/model.hpp"
#include "pixel/cache.hpp"
#include "render/backdrop.hpp"
#include "render/tile_look.hpp"
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

// Mission Control-style overview: a workspace strip over live window previews,
// drawn compositor-side while the real windows are hidden.
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
  void toggleAllWorkspaces();
  void toggleCarousel();   // flip the run on/off; on_open picks the start
  void altTabInvoke(bool reverse); // opens+seeds if closed, else advances

  // renderStage is the frame entry (BUILD); paint is the painter it schedules.
  void renderStage(eRenderStage stage);
  void paint();

  void renderBackdrop() const;
  // `live` = playing video: re-blur every frame, no cache.
  SP<Render::ITexture> backdropSource(bool &live) const;
  SP<Render::ITexture> renderBackdropSource(int W, int H) const;

  bool stripSlotGrabbed(size_t i, size_t j) const;
  void renderStrip() const;
  void renderStripWindows() const;
  void renderStripButtons() const;
  void renderPulses(bool strip) const;
  void renderTiles() const;
  // ONE pass: each overlapping card is a whole tile, chrome through content.
  void renderCarousel() const;
  void renderCarouselCounter() const;
  bool carouselMode() const;
  void stepCarousel(int dir); // center the next/previous card, circular
  CarouselLayout m_carousel; // written by layoutTiles
  // The selection m_carousel was built for; a change needs a relayout.
  int m_carouselFor = -1;
  void relayoutCarousel();
  // What layoutTiles resolved on its last call — the renderer's only source.
  Engine m_engine = Engine::Rows;
  bool   m_run = false; // the carousel run is on top of m_engine right now
  void renderGhosts() const;
  void renderDragTile() const;
  void renderDragWindow() const;
  void renderCursorOnTop() const;

  // An EVENT-LOOP timer: build-time damage raced the damage snapshot.
  void rearmanim() const;
  void ensureAnimPump();
  SP<CEventLoopTimer> m_animPump; // null when no animation is running

  // From the painter's tail; the immediate route reads m_tiles mid-draw.
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
  // Monotonic RENDER_LAST_MOMENT count, ticked while inactive.
  [[nodiscard]] std::uint64_t frameTick() const { return m_frameTick; }
  [[nodiscard]] PHLMONITOR monitor() const { return m_monitor.lock(); }
  [[nodiscard]] bool blurEnabled() const;
  [[nodiscard]] SP<Render::ITexture> backdropBlurTexture() const {
    return (m_blur.fb && m_blur.fb->isAllocated()) ? m_blur.fb->getTexture()
                                                   : nullptr;
  }
  [[nodiscard]] const std::unordered_map<void *, SP<Render::ITexture>> &
  snapshots() const {
    return m_pixel.snapshots();
  }
  [[nodiscard]] bool snapshotMode() const;

private:
  HANDLE m_handle = nullptr;
  bool m_active = false;
  bool m_opening = false;
  std::chrono::steady_clock::time_point m_openStamp{}; // frame-trace t=0
  std::uint64_t m_frameTick = 0;

  // The close hit 0 THIS frame and shouldRenderWindow already ran, so flipping
  // m_active off now skips the overlay: one blank frame.
  bool m_pendingDeactivate = false;
  bool m_pendingJumpClose  = false; // set after a jump ws-slide
  // The domain of time: every tween lives here and nowhere else.
  clock::Clocks m_clocks;
  double m_progress = 0.0;
  mutable std::chrono::steady_clock::time_point m_lastAnimTick{};
  // Any layout change is "retarget + restart". Empty oldBoxes = fresh naturals.
  void startTileGlide(
      const std::vector<std::pair<PHLWINDOW, LRect>> &oldBoxes);
  PHLMONITORREF m_monitor;
  PHLWORKSPACEREF m_workspace;
  PHLWORKSPACEREF m_liveWsAtOpen; // for exit_on_switch
  std::vector<model::Tile> m_tiles;
  std::vector<model::StripItem> m_strip;
  // mutable: capture is lazy, the first caller of a width renders it.
  mutable pixel::PixelCache m_pixel;
  double m_scrollAccum = 0.0; // sub-card wheel leftover, so slow paging still moves
  std::vector<std::pair<PHLLSREF, float>> m_hiddenLayers; // + pre-hide alpha
  int m_hovered = -1;      // index into m_tiles
  int m_hoveredStrip = -1;
  int m_selected = -1;     // keyboard-nav cursor into m_tiles
  // Committed focus target: a ws change re-asserts focus from its own history,
  // which can still name the old window.
  PHLWINDOWREF m_pendingFocus;

  // free-arrange: tiles at the real positions, drag floats the real window.
  bool m_desktopMode = false;
  int m_allOverride = -1; // -1 follow config, 0 off, 1 on
  double m_desktopS = 1.0;  // monitor->preview scale (inverse for drops)
  double m_desktopOx = 0.0;
  double m_desktopOy = 0.0;

  int m_newCardId = 0;
  bool m_newCardAnim = false;
  std::vector<PHLWORKSPACEREF> m_newWorkspaces;
  double m_stripScroll = 0.0;    // animated card-group offset on its main axis
  double m_stripScrollMax = 0.0;
  double m_stripScrollTarget = 0.0;
  double m_stripScrollFrom = 0.0;
  void animateStripTo(double from, double to);
  std::vector<model::Ghost> m_ghosts;
  anim::AnimCfg m_glide, m_entry, m_ghost, m_lift;
  std::string m_enterStyle, m_exitStyle;
  bool animBusy() const;
  // The clocks that outlive the master: the pump has to name every clock or the
  // next one added never arms it.
  bool secondaryAnimsActive() const {
    return m_clocks.anyRunning(leaf(entryLeaf()).ms, leaf(ghostLeaf()).ms,
                               leaf("strip_step").ms);
  }
  double tileAppear(int i) const; // staggered 0..1 for tile i
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

  // close_trigger == "doubleclick": the first click waits behind a timer, and that
  // armed timer IS the record. A second click before it fires closes the window.
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

  // Blur cache: frame one's blur lives in a persistent FBO, later frames blit it.
  // srcId + recipe compared EVERY frame, outside a dirty guard.
  mutable render::BlurCache m_blur;
  // Full-res FBO holding a rendered desktop background when there is no direct
  // texture to blur.
  mutable SP<Render::IFramebuffer> m_backdropSrcFB;
  mutable bool m_backdropDrawn = false;
  mutable std::chrono::steady_clock::time_point m_lastBackdropCapture{};
  mutable CBlurFilter m_blurFilter; // independent of decoration:blur:*
  void updateSnapshots();
  std::string cursorMode() const;

  anim::AnimCfg leaf(const char *name) const;

  enum class Anchor { Top, Bottom, Left, Right };
  Anchor stripAnchor() const; // falls back to bar_position
  bool stripHorizontal() const;
  double stripThickness() const; // perpendicular to the anchored edge
  double stripOffset() const;
  LRect stripBand() const;
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
  // Boundary policy (user-specified): real windows hard-switch at BOTH edges, NO
  // per-window fades — a fading original under the preview reads as double vision.
  double eased() const; // chrome opacity / backdrop progress
  double animDuration() const; // read LIVE, so config applies in-flight
  double glideDur() const { return m_clocks.tileMs > 0 ? m_clocks.tileMs : leaf(glideLeaf()).ms; }
  double newCardDur() const { return std::max(120.0, leaf("new_card").ms); }
  double tileProgress(int i) const;
  LRect currentBox(const model::Tile &t, int i) const;
  LRect dragBox() const;
  LRect dragStripBox() const;
  double dragLiftProgress() const;
  LRect dragVisualBox() const;
  int draggedTile() const;
  void dropLiftIfNotMoved();
  void drawTile(size_t i, const TileLook &look) const;
  // Every layout resolves its slot through this, and so does every hit-test.
  LRect tileBox(size_t i, const LRect &slot) const;
  void switchToWorkspace(const model::StripItem &it, bool instant = false);
  void dropOnWorkspace(const PHLWINDOW &w, const model::StripItem &it);
  // Shared tail of both release paths: a point on another card moves w there, or
  // swaps with its last-focused window (RMB).
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
  void drawPulseRing(const CBox &boxPx, int round, float roundPow,
                     const CHyprColor &col, double p) const;
  void addWorkspace();
  void closeWorkspaceWindows(const model::StripItem &it);
  void setDesktopMode(bool on); // flip grid<->canvas, purely visual
  LRect closeButtonRect(const LRect &tile) const;
  bool closeButtonsAlwaysOn() const;
  bool closeOnDoubleClick() const;
  bool tileCloseButtons() const;
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
  // A target off the DISPLAYED workspace collapses onto it and rebuilds BEFORE
  // the glide, so the glide aims at the real final position.
  void focusAndClose(const PHLWINDOW &w, Desktop::eFocusReason reason);
  bool crossesRealWorkspace(const PHLWINDOW &w) const;
  void syncFocus() const;
  void closeTileWindow(int i);
  // Drop zone: opposite the strip's anchored edge. No hit test of its own — the
  // release and the hover both go through killZoneRect, so drawn and accepted
  // areas cannot drift.
  LRect killZoneRect() const;
  double killZoneGlow(double lx, double ly) const;
  void renderKillZone() const;
  double m_killGlow = 0.0; // 1 at the zone, 0 a screen-width away, while dragging
  // Carousel on/off for the all-workspaces view. layoutTiles reads this, not the
  // config, so a toggle is not overwritten by the next layout.
  bool m_carouselOn = true;
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
