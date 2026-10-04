#pragma once

// The animation clock primitive + the per-leaf config snapshot. Pure time math
// only — shaping lives in anim/curves.cpp.

#include <algorithm>
#include <chrono>
#include <string>

namespace gloview::anim {

// Monotonic anchor for a hand-driven clock. Durations live at the call sites
// (picked up live, even mid-animation); the tween owns only WHEN the clock
// started plus the raw 0..1 math.
struct Tween {
  std::chrono::steady_clock::time_point start{};
  mutable double last = 0.0; // last value raw() returned

  // A fresh run resets `last` too: the stall guard rewinds to it, so a stale
  // 1.0 from an idle period would snap the first post-begin frame to the end.
  void begin() {
    start = clock::now();
    last = 0.0;
  }
  // Discard wall-time spent with no frames being produced: re-anchor at the last
  // known pre-gap value (re-anchoring at the current raw would be a no-op — it
  // already contains the hole).
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

// One animation group's config, resolved per frame by Overview::leaf().
struct AnimCfg {
  bool on = false;
  double ms = 1.0;
  std::string curve = "easeout";
};

} // namespace gloview::anim
