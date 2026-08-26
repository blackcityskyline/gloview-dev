#pragma once

// The animation clock primitive + the per-leaf config snapshot (REFACTORING.md
// A1/M1). Pure time math lives here; shaping happens in anim/curves.cpp.

#include <algorithm>
#include <chrono>
#include <string>

namespace gloview::anim {

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

// Snapshot of one animation group's config, resolved per frame by
// Overview::leaf() from cfg::anim.
struct AnimCfg {
  bool on = false;
  double ms = 1.0; // resolved window: >= 1, never the -1 sentinel
  std::string curve = "easeout";
};

} // namespace gloview::anim
