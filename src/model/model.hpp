#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include "../clock/clocks.hpp"
#include "../layout.hpp"

namespace gloview::model {

struct Tile {
  PHLWINDOWREF win;
  LRect natural;
  LRect target;
  bool parked = false;
  std::string labelText;
  void *labelKey = nullptr; // cache key for the caption; null = nothing to fit
  double appear = 1.0;  // 0 = new; ghosts reverse it
  SP<Render::ITexture> label;
};

struct StripWin {
  PHLWINDOWREF win;
  LRect rel;
};

struct StripItem {
  enum class Kind : int { Ws, Plus, All };
  PHLWORKSPACEREF ws;
  int id = 0;
  bool active = false;
  Kind kind = Kind::Ws;
  // id precedes its PHLWORKSPACE object
  bool virtualWs = false;
  LRect card;
  std::vector<StripWin> wins;
  SP<Render::ITexture> label;
};

struct LabelTex {
  std::string text;
  SP<Render::ITexture> tex;
};

struct Ghost {
  PHLWINDOWREF win;
  LRect box;
};

// `p` accrues per FRAME, not wall-clock
struct WinPulse {
  PHLWINDOWREF w;
  double p = 0.0;
  std::chrono::steady_clock::time_point last;
};

// visuals gate on `lifted`, not `press`
struct Drag {
  enum class Press : int {
    Tile,
    StripWin,
    Empty,
    Consumed,
  };
  Press press = Press::Empty;
  int idx = -1, winIdx = -1;
  PHLWINDOWREF win;
  int button = 0;
  double pressX = 0, pressY = 0;
  double grabDX = 0, grabDY = 0;
  double x = 0, y = 0;
  bool lifted = false;
  std::chrono::steady_clock::time_point holdStartMs;
  LRect fromBox;
  bool armed() const {
    return press == Press::Tile || press == Press::StripWin;
  }
  double travelSq() const {
    const double dx = x - pressX, dy = y - pressY;
    return dx * dx + dy * dy;
  }
};

enum class SwapStyle : int { Horizontal, SlideVert, Fade, Pop };

struct SwapFX {
  PHLWINDOWREF win;
  LRect from;
  SwapStyle style = SwapStyle::Horizontal;
  double ms = 320;
  std::string curve = "easeinout";
  anim::Tween clock;
};

}
