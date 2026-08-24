#pragma once

// The Model store's data structures (REFACTORING.md M1): what EXISTS in the
// overlay. Pure data — no Hyprland calls, no rendering. Whoever mutates these
// (build/input/actions) owns the state transition; the painter only reads.

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

// Rendered-text cache entry. Tiles and StripItems are RECREATED on every
// rebuild (each drop/swap/sync), so caching on them re-rasterized every
// label every drop — the drag&drop stutter. Keyed by the owning
// window/workspace pointer with mark-and-sweep per build pass (explicit
// lifecycle, per AGENTS).
struct LabelTex {
  std::string text;
  SP<Render::ITexture> tex;
};

struct Ghost {
  PHLWINDOWREF win;
  LRect box; // monitor-local logical, frozen at removal
};

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

// A drag/swap landing: the window's content flies from `from` (the box it
// occupied at release — the drag preview under the cursor, or its old slot)
// into its new slot. While the flight is live the painter draws the window
// ONCE above the strip and suppresses its regular slot rendering; at t=1 the
// lerped box equals the slot box, so the handoff to normal rendering is
// invisible.
struct Landing {
  PHLWINDOWREF win;
  LRect from; // monitor-local logical
  anim::Tween clock;
};

} // namespace gloview::model
