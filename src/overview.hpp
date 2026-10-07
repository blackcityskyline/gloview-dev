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

#include "anim.hpp"
#include "clock/clocks.hpp"
#include "clock/clock.hpp"
#include "view/state.hpp"
#include "model/effects.hpp"
#include "model/recency.hpp"
#include "model/scene.hpp"
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

  void renderStage(eRenderStage stage);
  void paint();

  void renderBackdrop() const;
  void primeBackdrop();
  SP<Render::ITexture> renderBackdropSource(int W, int H) const;

  bool stripSlotGrabbed(size_t i, size_t j) const;
  void renderStrip() const;
  void renderStripWindows() const;
  void renderStripButtons() const;
  void renderPulses(bool strip) const;
  void renderTiles() const;
  void renderCarousel() const;
  void renderCarouselCounter() const;
  bool carouselMode() const;
  void relayoutCarousel();
  void renderGhosts() const;
  void renderDragTile() const;
  void renderDragWindow() const;
  void renderCursorOnTop() const;

  void rearmanim() const;
  void ensureAnimPump();
  SP<CEventLoopTimer> m_animPump; // null when no animation is running

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
  // open() parks before it becomes visible, so "did the open take" is active OR parked.
  // Every caller that tests m_active right after open() must test this instead, or it reads
  // a waiting open as a failed one -- that is what silently disarmed alt+tab.
  // Also the predicate for input ownership: a parked frame already covers the windows.
  [[nodiscard]] bool openedOrParked() const { return m_active || m_pendingOpen; }
  [[nodiscard]] bool opening() const { return m_opening; } // entry anim only
  [[nodiscard]] std::uint64_t frameTick() const { return m_frameTick; }
  [[nodiscard]] PHLMONITOR monitor() const { return m_monitor.lock(); }
  [[nodiscard]] bool blurEnabled() const;
  [[nodiscard]] SP<Render::ITexture> backdropBlurTexture() const {
    return (m_blur.fb && m_blur.fb->isAllocated()) ? m_blur.fb->getTexture()
                                                   : nullptr;
  }
  [[nodiscard]] SP<Render::ITexture> snapshot(const PHLWINDOW &w) const {
    return m_pixel.snapshot(w);
  }
  [[nodiscard]] bool snapshotMode() const;

private:
  HANDLE m_handle = nullptr;
  bool m_active = false;
  bool m_opening = false;
  std::chrono::steady_clock::time_point m_openStamp{}; // frame-trace t=0
  std::uint64_t m_frameTick = 0;

  // The close hit 0 THIS frame and shouldRenderWindow already ran, so flipping
  // m_active off now skips the overlay.
  bool m_pendingDeactivate = false;
  // Open in two phases: nothing is visible until a backdrop exists. open() parks here, the
  // pass computes the composite into a PRIVATE framebuffer for a frame or two (nothing is
  // drawn), and the first non-black composite promotes the open. Measured reason: the frame
  // the overview is first visible had no backdrop of its own, and the wallpaper texture is
  // mid-recreation right then, so it composed black. Painting late beats painting black.
  bool m_pendingOpen = false;
  int m_pendingTicks = 0;
  bool m_pendingJumpClose  = false; // set after a jump ws-slide
  model::Recency m_recency;
  PHLWINDOWREF m_lastFocus;
  // The compositor's focus when the overlay opened, held for its whole lifetime.
  // Inside, the run's selection is INTERNAL and never touches the compositor, so
  // hovering a card cannot change what a passthrough bind acts on.
  PHLWINDOWREF m_focusOutside;
  // A step the run must apply BEFORE it resolves its centre, so the opening
  // motion plays for the target window instead of landing on one and swapping to
  // another at the end. Consumed by layoutTiles, never by a key path.
  int m_pendingStep = 0;
  void syncRecency();
  void pruneRecency();
  model::Scene m_scene;
  model::Effects m_fx;
  view::State m_view;
  clock::Clocks m_clocks;
  double m_progress = 0.0;
  mutable std::chrono::steady_clock::time_point m_lastAnimTick{};
  void startTileGlide(
      const std::vector<std::pair<PHLWINDOW, LRect>> &oldBoxes);
  PHLMONITORREF m_monitor;
  PHLWORKSPACEREF m_workspace;
  PHLWORKSPACEREF m_liveWsAtOpen; // for exit_on_switch
  std::vector<model::StripItem> m_strip;
  mutable pixel::PixelCache m_pixel;
  double m_scrollAccum = 0.0; // sub-card wheel leftover, so slow paging still moves
  std::vector<std::pair<PHLLSREF, float>> m_hiddenLayers; // + pre-hide alpha
  PHLWINDOWREF m_pendingFocus;


  int m_newCardId = 0;
  bool m_newCardAnim = false;
  double m_stripScroll = 0.0;    // animated card-group offset on its main axis
  double m_stripScrollMax = 0.0;
  double m_stripScrollTarget = 0.0;
  double m_stripScrollFrom = 0.0;
  void animateStripTo(double from, double to);
  anim::Cfg m_glide, m_entry, m_ghost, m_lift;
  std::string m_enterStyle, m_exitStyle;
  bool animBusy() const;
  // Clocks that outlive the master: the pump must name every clock or the next one
  // added never arms it.
  bool secondaryAnimsActive() const {
    return m_clocks.anyRunning(anim::cfg(entryLeaf()).ms, anim::cfg(ghostLeaf()).ms,
                               anim::cfg(anim::Id::StripStep).ms);
  }
  // Read on the call site: a config edit mid-fade retimes the rest of the ramp.
  double blurAlpha() const; // backdrop strength, on the open/close clock
  // The predicate build.cpp/actions.cpp each re-spelled: a window that really occupies
  // the desktop. First true one on `ws` is what tells an empty workspace from a busy one.
  static bool workspaceHasWindows(const PHLWORKSPACE &ws);
  // Windowed exit is done: tiles landed and Hyprland has spent the forced blur frames.
  bool backdropSettled() const;
  // Landing workspace empty? Re-read on every open and close: the landing workspace can
  // change while the overview is up (all-workspaces, digit jump).
  mutable bool m_landingEmpty = true;
  double tileAppear(int i) const; // staggered 0..1 for tile i
  anim::Id entryLeaf() const;
  anim::Id ghostLeaf() const;
  anim::Id glideLeaf() const;
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
  // armed timer IS the record. A second click before it fires closes it.
  PHLWINDOWREF m_pendingClickWin;
  SP<CEventLoopTimer> m_clickTimer;

  model::Drag m_drag; // pointer PRESS..RELEASE, both drag kinds
  LRect m_lastDragBox; // drag box at RELEASE, the `from` for landings
  bool m_altTabbing = false;
  bool m_commitPending = false;

  CHyprSignalListener m_renderStageL;
  CHyprSignalListener m_mouseButtonL;
  CHyprSignalListener m_mouseAxisL;
  CHyprSignalListener m_mouseMoveL;
  CHyprSignalListener m_keyL;
  CFunctionHook *m_shouldRenderHook = nullptr;
  CFunctionHook *m_damageSurfaceHook = nullptr;
  CFunctionHook *m_setPointerFocusHook = nullptr;
  CFunctionHook *m_setKeyboardFocusHook = nullptr;

  CCursorModule m_cursor; // KMS plane preferred, software erases-and-redraws

  mutable render::BlurCache m_blur;
  mutable SP<Render::IFramebuffer> m_backdropSrcFB;
  // New composites are built here and only adopted once they read as a real picture, so a
  // black attempt can never overwrite the good one.
  mutable SP<Render::IFramebuffer> m_backdropScratchFB;
  mutable bool m_backdropDrawn = false;
  // The layer texture the saved composite was made from. Compared on every open, so the
  // composite can be kept indefinitely instead of expiring on a timer: a TTL cannot tell
  // "the desktop changed" from "the wallpaper texture is mid-recreation right now".
  mutable const void *m_backdropSrcTex = nullptr;
  // A video wallpaper replaces its source texture every frame. Detected from consecutive
  // changes, so a one-off compositor re-creation of a static wallpaper is not mistaken for it.
  // Such a source is composited once and then held: one frozen frame of the video, shown for
  // the whole open. Its black-frame check is skipped -- a dark video frame is real content,
  // not "not ready yet".
  mutable const void *m_lastObservedTex = nullptr;
  mutable int m_srcAnimStreak = 0;
  mutable bool m_srcAnimated = false;
  mutable CBlurFilter m_blurFilter; // independent of decoration:blur:*
  void updateSnapshots();
  std::string cursorMode() const;


  enum class Anchor { Top, Bottom, Left, Right };
  Anchor stripAnchor() const; // falls back to bar_position
  bool stripHorizontal() const;
  double stripThickness() const; // perpendicular to the anchored edge
  double stripOffset() const;
  LRect stripBand() const;
  Vector2D stripSlide(double e) const; // reveal slide-in at progress e
  Vector2D stripPush(const LRect &band) const; // band yields to a preview that reached it
  // THE strip displacement. Band, cards, the window previews inside them and the hit-test all
  // go through this: computing it in each of them meant the previews and the hit-test drifted
  // from the cards by exactly the push (measured: window previews stopped following the
  // motion on any workspace that HAS windows).
  Vector2D stripOffset(double e) const;
  Vector2D pushOut(const LRect &r) const; // chrome yields to a preview
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
  double glideDur() const { return m_clocks.tileMs > 0 ? m_clocks.tileMs : anim::cfg(glideLeaf()).ms; }
  bool entrySettled() const;
  uint32_t altTabModBit() const;
  double newCardDur() const { return anim::cfg(anim::Id::NewCard).ms; }
  double tileProgress(int i) const;
  LRect currentBox(const model::Tile &t, int i) const;
  LRect dragBox() const;
  LRect dragStripBox() const;
  double dragLiftProgress() const;
  LRect dragVisualBox() const;
  int draggedTile() const;
  void dropLiftIfNotMoved();
  void drawTile(size_t i, const TileLook &look) const;
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
  // the glide, so it aims at the real final position.
  void focusAndClose(const PHLWINDOW &w, Desktop::eFocusReason reason);
  bool crossesRealWorkspace(const PHLWINDOW &w) const;
  void closeTileWindow(int i);
  LRect killZoneRect() const;
  double killZoneGlow(double lx, double ly) const;
  void renderKillZone() const;
  double m_killGlow = 0.0; // 1 at the zone, 0 a screen-width away, while dragging
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
  int tileOf(const PHLWINDOW &w) const;
  void dbg(const std::string &msg) const;
  void damage() const;
};

} // namespace gloview

inline gloview::Overview *g_overview = nullptr;
