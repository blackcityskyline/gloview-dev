#pragma once

#include <algorithm>
#include <chrono>
#include <string>

namespace gloview::anim {

struct Tween {
  std::chrono::steady_clock::time_point start{};
  mutable double last = 0.0;

  // begin resets last; stall guard rewinds to it
  void begin() {
    start = clock::now();
    last = 0.0;
  }
  // re-anchor at `last`, not current raw
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

struct AnimCfg {
  bool on = false;
  double ms = 1.0;
  std::string curve = "easeout";
};

}
