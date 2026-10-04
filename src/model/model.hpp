#pragma once

// What EXISTS in the overlay. Pure data — no Hyprland calls, no rendering.
// build/input/actions own the mutations; the painter only reads.

#include <chrono>
#include <string>
#include <vector>

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include "../anim/clocks.hpp"
#include "../layout.hpp"

namespace gloview::model {

struct Tile {
  PHLWINDOWREF win;
  LRect natural; // monitor-local logical: real place (goal); animation start
  LRect target;  // monitor-local logical: grid slot
  bool parked = false; // canvas mode: user-placed, only its own drag moves it
  std::string labelText; // cache key for `label`
  double appear = 1.0;   // 0 = new to this rebuild; ghosts cover the reverse
  SP<Render::ITexture> label;
};

struct StripWin {
  PHLWINDOWREF win;
  LRect rel; // 0..1 slot within the card; rendered from the LIVE surface
};

struct StripItem {
  enum class Kind : int { Ws, Plus, All }; // All = leading expo-toggle card
  PHLWORKSPACEREF ws;
  int id = 0;
  bool active = false;
  Kind kind = Kind::Ws;
  // Placeholder for a numeric ID with no PHLWORKSPACE object yet (Hyprland
  // only keeps objects it has created). `ws` stays unset; clicking/dropping
  // creates the real workspace at exactly `id`.
  bool virtualWs = false;
  LRect card; // monitor-local logical
  std::vector<StripWin> wins;
  SP<Render::ITexture> label;
};

// Rendered-text cache. Tiles/StripItems are recreated on every rebuild, so
// caching on them re-rasterized every label per drop — the drag stutter. Keyed
// by the owning window/workspace pointer, swept per build pass.
struct LabelTex {
  std::string text;
  SP<Render::ITexture> tex;
};

struct Ghost {
  PHLWINDOWREF win;
  LRect box; // monitor-local logical, frozen at removal
};

// Swap success ring. Window-keyed so a rebuild between kick and render can't
// orphan it. `p` accumulates per FRAME, not wall-clock: a heavy frame right
// after a drop must not leap the easeOutBack plateau.
struct WinPulse {
  PHLWINDOWREF w;
  double p = 0.0;
  std::chrono::steady_clock::time_point last;
};

// Everything the pointer did between PRESS and RELEASE, for both drag kinds and
// every press outcome. One value, so a stale half-armed drag cannot survive a
// session, and release logic switches on `press` instead of decoding sentinels.
//
// RULE: the drag visual (renderDragWindow, drawDragStripChrome,
// suppressContent) is gated on `lifted` everywhere except the grab ring —
// press alone means "armed, nothing drawn yet".
struct Drag {
  enum class Press : int {
    Tile,      // main-grid tile: idx = m_tiles index
    StripWin,  // strip card's window slot: idx = m_strip index, winIdx = its
               // index into it, win = the window
    Empty,     // empty space → close on release unless consumed
    Consumed,  // handled on the spot (e.g. a ✕ button)
  };
  Press press = Press::Empty;
  int idx = -1, winIdx = -1;
  PHLWINDOWREF win;
  int button = 0;
  double pressX = 0, pressY = 0;   // monitor-local press point
  double grabDX = 0, grabDY = 0;   // cursor offset inside the grabbed box
  double x = 0, y = 0;             // current cursor, kept fresh by updateHover
  bool lifted = false;             // past the threshold → a real drag
  std::chrono::steady_clock::time_point holdStartMs;
  LRect fromBox;                   // grabbed box at lift = flight origin
  bool armed() const {
    return press == Press::Tile || press == Press::StripWin;
  }
  double travelSq() const {
    const double dx = x - pressX, dy = y - pressY;
    return dx * dx + dy * dy;
  }
};

// How a window leaves its old box and enters the new one.
enum class SwapStyle : int { Horizontal, SlideVert, Fade, Pop };

// A drag/swap landing: content flies from `from` into its new slot. While live
// the painter draws it ONCE above the strip and suppresses the slot render; at
// t=1 the lerped box equals the slot box, so the handoff is invisible.
struct SwapFX {
  PHLWINDOWREF win;
  LRect from;    // monitor-local logical
  SwapStyle style = SwapStyle::Horizontal;
  double ms = 320;
  std::string curve = "easeinout";
  anim::Tween clock;
};

} // namespace gloview::model
