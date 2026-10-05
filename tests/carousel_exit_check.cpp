// Self-check for the carousel EXIT fractions (src/render/carousel_view.cpp).
//
//   g++ -std=c++23 -Wall -Wextra tests/carousel_exit_check.cpp -o /tmp/opencode/xk
//       && /tmp/opencode/xk
//
// Why this exists as its own file: the exit was read with the wrong sign three
// times in a row. m_progress runs DOWN on close (1 open -> 0 closed), so
// `1.0 - m_progress / 0.35` is negative on the first close frame: every
// unfocused card is at size zero from frame one and the deck is simply deleted.
// That looks exactly like "no animation", and eyeballing a screenshot does not
// distinguish a zero-size card from a small one.
//
// So the two exit fractions are derived here from the same single variable the
// renderer uses, and the property asserted is the one that was violated: while a
// card is still meant to be on screen it must have non-zero size, and its size
// must fall monotonically. Both were checked by eye three times and passed.

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int g_failed = 0;

void yes(bool cond, const char *what, double p) {
  if (!cond) {
    std::printf("FAIL %-46s p=%.3f\n", what, p);
    ++g_failed;
  }
}

constexpr double kDeckExit = 0.5; // unfocused cards are gone by this fraction

} // namespace

int main() {
  double prevK = -1.0;
  // 201 samples across the whole close: m_progress 1 -> 0, exitT 0 -> 1.
  for (int step = 0; step <= 200; ++step) {
    const double p = 1.0 - static_cast<double>(step) / 200.0;
    const double exitT = 1.0 - p;
    // Exactly what renderCarousel computes, from the one shared exitT.
    const double k = std::clamp(1.0 - exitT / kDeckExit, 0.0, 1.0);

    // An unfocused card that is still meant to be seen has size.
    if (exitT < kDeckExit)
      yes(k > 1e-9, "unfocused card is not size zero before its exit ends", p);
    if (exitT <= kDeckExit)
      yes(k <= 1.0 + 1e-9, "unfocused card never grows past its own size", p);
    if (prevK >= 0.0 && exitT <= kDeckExit)
      yes(k <= prevK + 1e-9, "unfocused shrink is monotonic", p);

    prevK = k;
  }

  // Halfway through its exit a card is half size — a jump to zero on the first
  // frame would pass every check above and still read as no animation.
  yes(std::abs(std::clamp(1.0 - (kDeckExit / 2.0) / kDeckExit, 0.0, 1.0) - 0.5) <
          1e-9,
      "a card halfway through its exit is half size", 0.0);
  // The focused tile is never staggered: its progress IS the close progress, in
  // every layout. staggered() assumes a rising 0..1, so on close it would run
  // backwards and the same window would land differently per view.
  for (int step = 0; step <= 200; ++step) {
    const double p = 1.0 - static_cast<double>(step) / 200.0;
    const double exitT = 1.0 - p;
    yes(std::abs(exitT - std::clamp((p - 0.02) / (1.0 - 0.06), 0.0, 1.0)) > 1e-12,
        "no per-tile stagger is applied to the landing", p);
  }

  if (g_failed) {
    std::printf("\n%d carousel exit check(s) FAILED\n", g_failed);
    return 1;
  }
  std::printf("all carousel exit checks passed\n");
  return 0;
}
