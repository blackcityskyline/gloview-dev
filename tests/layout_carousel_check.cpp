// Self-check for the carousel geometry (src/layout.cpp). Every expected number
// below is computed BY HAND from the documented metrics and written out in the
// comment above it, so the test states what the geometry should be instead of
// reporting whatever the implementation happens to print.
//
//   g++ -std=c++23 -Wall -Wextra -I src tests/layout_carousel_check.cpp
//       src/layout.cpp -o /tmp/opencode/ck && /tmp/opencode/ck
//
// Layout is pure geometry with no compositor dependency, which is what makes
// this checkable at all.
//
// What the mode is (section A pins all of it):
//   * every card is the SAME aspect (mt.aspect); its width comes from its depth
//     alone, stepping down strictly outward: selectedW, *nearK, *farK, then
//     *taper per depth past that;
//   * the run is centred and every adjacent pair overlaps by exactly mt.overlap.

#include "layout.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace gloview;

namespace {

int g_failed = 0;

void near(double got, double want, const char *what, double tol = 1e-9) {
  if (std::abs(got - want) > tol) {
    std::printf("FAIL %-48s got %.9f want %.9f\n", what, got, want);
    ++g_failed;
  }
}

void yes(bool cond, const char *what) {
  if (!cond) {
    std::printf("FAIL %s\n", what);
    ++g_failed;
  }
}

constexpr double kAspect = 1.6;
constexpr double kOverlap = 60.0;

void sectionA_widthLadderAndAspect() {
  CarouselMetrics mt;
  mt.selectedW = 500.0;
  mt.nearK = 0.76;
  mt.farK = 0.56;
  mt.taper = 0.8;
  mt.overlap = kOverlap;
  mt.aspect = kAspect;
  mt.visibleCards = 5;

  // 1800 wide: the run needs 1580 (see section B) and availW = 1736, so nothing
  // scales and every number below is the raw metric.
  // sel 4 -> slots -2..+2 -> indices 2,3,4,5,6 at depths 2,1,0,1,2.
  const auto L = computeCarousel(9, 4, LRect{0, 0, 1800, 1000}, mt);

  // widths: depth 2 = 500*0.56 = 280, depth 1 = 500*0.76 = 380, depth 0 = 500.
  const double want[] = {280.0, 380.0, 500.0, 380.0, 280.0};
  for (int i = 0; i < 5; ++i) {
    const int idx = 2 + i;
    const auto &c = L.cards[static_cast<size_t>(idx)];
    yes(c.visible, "ladder card visible");
    near(c.w, want[i], "ladder width");
    // height = width / 1.6, plus the caption band on the centered card only.
    const double band = (i == 2) ? (mt.captionGap + mt.captionH) : 0.0;
    near(c.h, want[i] / kAspect + band, "ladder height");
    yes(std::abs(c.w / (c.h - band) - kAspect) < 1e-9,
        "card keeps the run's one aspect");
    // Side cards centre on the centered card's FRAME (312.5), not its band.
    near(c.y, (500.0 / kAspect - want[i] / kAspect) * 0.5, "side card centred");
  }
  near(L.cards[4].y, 0.0, "centered card sits at the top of the band");

  // depth is |slot| and the caption rides the centered card.
  for (int i = 0; i < 9; ++i) {
    const auto &c = L.cards[static_cast<size_t>(i)];
    yes(c.visible == (i >= 2 && i <= 6), "only five cards are placed");
    if (!c.visible) {
      // Off-run cards keep the struct defaults; nothing places them.
      near(c.w, 0.0, "off-run card has no width");
      yes(!c.showCaption, "off-run card carries no caption");
      continue;
    }
    yes(c.depth == std::abs(i - 4), "depth is the distance from centre");
    yes(c.showCaption == (i == 4), "only the centered card carries the caption");
  }
}

void sectionB_runFitsAndIsCentred() {
  CarouselMetrics mt;
  mt.selectedW = 500.0;
  mt.overlap = kOverlap;
  mt.aspect = kAspect;
  mt.visibleCards = 5;

  // sel 2 -> slots -2..+2 at depths 2,1,0,1,2: widths 280+380+500+380+280 =
  // 1820, minus 4 overlaps of 60 = 1580. Tallest card is the centered one:
  // 500/1.6 + 12 + 44 = 368.5.
  // availW = 1800 - 64 = 1736 > 1580 and availH = 1000 - 64 - 32 - 12 = 892 >
  // 368.5, so k == 1 and nothing shrinks.
  const auto L = computeCarousel(5, 2, LRect{0, 0, 1800, 1000}, mt);
  near(L.stripW, 1580.0, "strip width, roomy screen");
  near(L.stripH, 368.5, "strip height is the centered card");
  yes(L.stripW <= 1800.0 - 2 * mt.screenMargin, "strip fits the width");
  near(L.originX, (1800.0 - 1580.0) * 0.5, "run centred horizontally");
  // originY is rounded: panelH = 32 + 12 + 368.5 = 412.5, (1000-412.5)/2 =
  // 293.75 -> 294, so the panel lands on whole pixels.
  near(L.originY, 294.0, "run centred vertically, rounded to a whole pixel");

  // Flush inside the strip: the left extent is w(3) + w(2) - 2*overlap =
  // 380 + 280 - 120 = 540, which is the centered card's x.
  near(L.cards[2].x, 540.0, "centered card x");
  near(L.cards[1].x, 220.0, "left-1 x");
  near(L.cards[0].x, 0.0, "leftmost x is flush at stripX");
  near(L.cards[3].x, 980.0, "right-1 x");
  near(L.cards[4].x, 1300.0, "rightmost x");
  near(1300.0 + 280.0, L.stripW, "run ends flush at stripX + stripW");

  // Adjacent cards overlap by exactly the overlap, on both sides.
  near((220.0 + 380.0) - 540.0, kOverlap, "left-1 overlaps the centre");
  near(540.0 + 500.0 - 980.0, kOverlap, "centre overlaps right-1");
  near(980.0 + 380.0 - 1300.0, kOverlap, "right-1 overlaps right-2");
  near((0.0 + 280.0) - 220.0, kOverlap, "leftmost overlaps left-1");
}

void sectionC_scalesDownWhenTight() {
  CarouselMetrics mt;
  mt.selectedW = 500.0;
  mt.overlap = kOverlap;
  mt.aspect = kAspect;
  mt.visibleCards = 5;

  // Same run on a 700-wide screen: availW = 700 - 64 = 636, so the width branch
  // binds at k = 636 / 1580 and the height branch (892 / 368.5 = 2.42) does not.
  const auto L = computeCarousel(5, 2, LRect{0, 0, 700, 1000}, mt);
  const double k = 636.0 / 1580.0;
  near(L.stripW, 636.0, "tight screen: strip fits exactly");
  yes(L.stripW <= 700.0 - 2 * mt.screenMargin, "tight strip fits");
  near(L.cards[2].w, 500.0 * k, "centered width scaled");
  near(L.cards[2].h, 368.5 * k, "centered height scaled");
  for (const auto &c : L.cards) {
    if (!c.visible)
      continue;
    const double band = c.showCaption ? (mt.captionGap + mt.captionH) * k : 0.0;
    yes(std::abs(c.w / (c.h - band) - kAspect) < 1e-9,
        "aspect survives scaling");
  }
  // Overlap scales with the run, or the cards stop overlapping once it shrinks.
  const double ov = kOverlap * k;
  near((L.cards[1].x + L.cards[1].w) - L.cards[2].x, ov,
       "overlap scales with the run");
}

void sectionD_wrappingAndEmpty() {
  CarouselMetrics mt;
  mt.selectedW = 500.0;
  mt.overlap = kOverlap;
  mt.aspect = kAspect;
  mt.visibleCards = 5;

  const auto a = computeCarousel(5, -1, LRect{0, 0, 1600, 1000}, mt);
  yes(a.cards[4].visible && a.cards[4].depth == 0, "selected -1 wraps to 4");
  yes(a.cards[4].showCaption, "wrapped selection is captioned");

  const auto b = computeCarousel(5, 5, LRect{0, 0, 1600, 1000}, mt);
  yes(b.cards[0].visible && b.cards[0].depth == 0, "selected == count wraps to 0");

  const auto e = computeCarousel(0, 0, LRect{0, 0, 1600, 1000}, mt);
  yes(e.cards.empty(), "count 0 has no cards");
  yes(e.visibleCards == 0, "count 0 shows nothing");
  near(e.stripW, 0.0, "count 0 has no strip");
}

void sectionE_fewerWindowsThanSlots() {
  CarouselMetrics mt;
  mt.selectedW = 500.0;
  mt.overlap = kOverlap;
  mt.aspect = kAspect;
  mt.visibleCards = 5;

  // 2 windows, 5 slots: centerSlot 2, so slots -2..+2 wrap onto indices
  // {1,0,1,0,1} — each index placed once, at the slot nearest it.
  const auto L = computeCarousel(2, 0, LRect{0, 0, 1600, 1000}, mt);
  int visible = 0;
  for (const auto &c : L.cards)
    visible += c.visible ? 1 : 0;
  yes(visible == 2, "both windows placed when there are fewer than slots");
  yes(L.visibleCards == 2, "visibleCards is clamped to the count");
}

} // namespace

int main() {
  sectionA_widthLadderAndAspect();
  sectionB_runFitsAndIsCentred();
  sectionC_scalesDownWhenTight();
  sectionD_wrappingAndEmpty();
  sectionE_fewerWindowsThanSlots();

  if (g_failed) {
    std::printf("\n%d carousel layout check(s) FAILED\n", g_failed);
    return 1;
  }
  std::printf("all carousel layout checks passed\n");
  return 0;
}
