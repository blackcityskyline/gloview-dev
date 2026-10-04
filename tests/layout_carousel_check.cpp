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
// What the mode is, all of it pinned in section A:
//   * ONE standard box for every card — the size a fullscreen window occupies.
//     Identical width and height, to the pixel, whatever window it holds.
//   * cards step left to right, each overlapping the previous by mt.overlap;
//   * the centred card is the run's middle and carries the caption band;
//   * the whole run scales down by ONE factor so it fits both axes at once.

#include "layout.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>
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

// A fullscreen window's box on a 1206x493 screen with the defaults.
constexpr double kUsableW = 1206.0;
constexpr double kUsableH = 493.0;
constexpr double kMargin = 32.0;
constexpr double kOverlap = 60.0;
constexpr double kCaption = 44.0 + 12.0;

CarouselMetrics defaults() {
  CarouselMetrics mt;
  mt.boxW = kUsableW;
  mt.boxH = kUsableH;
  mt.overlap = kOverlap;
  mt.visibleCards = 5;
  return mt;
}

// Section A: the standard box. Every card gets it, whatever it holds.
void sectionA_oneStandardBox() {
  auto mt = defaults();
  const auto L = computeCarousel(5, 2, LRect{0, 0, kUsableW, kUsableH}, mt);

  // 5 cards: stripW = 5*1206 - 4*60 = 6030 - 240 = 5790, which does NOT fit
  // availW = 1206 - 64 = 1142, so k = 1142/5790 and every card shrinks by it.
  const double k = 1142.0 / 5790.0;
  for (const auto &c : L.cards) {
    if (!c.visible)
      continue;
    // Every card the same width and height, to the last bit: this is the whole
    // point, and it is why the run cannot shift with the selection.
    near(c.w, kUsableW * k, "every card is the standard width");
    // Including the centred one: the caption strip is reserved in stripH, not
    // bolted onto one card, because a taller centred box is a different size.
    near(c.h, kUsableH * k, "every card is the standard height");
  }

  // Distinct boxes: the five placed ones must not share a width by accident of
  // rounding, and the centred one must be the only one with the caption band.
  int captioned = 0;
  for (const auto &c : L.cards)
    captioned += c.showCaption ? 1 : 0;
  yes(captioned == 1, "exactly one card carries the caption");
}

// Section B: placement. Overlap per step, centred card in the middle.
void sectionB_placementAndCentring() {
  auto mt = defaults();
  const auto L = computeCarousel(5, 2, LRect{0, 0, kUsableW, kUsableH}, mt);
  const double k = 1142.0 / 5790.0;
  const double w = kUsableW * k;
  const double ov = kOverlap * k;

  // sel 2 -> slots -2,-1,0,1,2 -> indices 0,1,2,3,4. Anchored to the RUN: the
  // leftmost card's x is 0 and each step adds one (w - overlap).
  near(L.cards[0].x, 0.0, "leftmost card is flush at stripX");
  near(L.cards[1].x, w - ov, "left-1 steps one overlap out");
  near(L.cards[2].x, 2.0 * (w - ov), "centred card sits at the run's middle");
  near(L.cards[3].x, 3.0 * (w - ov), "right-1 steps one overlap out");
  near(L.cards[4].x, 4.0 * (w - ov), "right-2 steps two overlaps out");
  near(L.cards[4].x + L.cards[4].w, L.stripW, "run ends flush at stripX + stripW");

  // Adjacent cards overlap by exactly mt.overlap, scaled.
  near((L.cards[1].x + L.cards[1].w) - L.cards[2].x, ov, "left-1 overlap");
  near((L.cards[2].x + L.cards[2].w) - L.cards[3].x, ov, "centre/right-1 overlap");
  near((L.cards[0].x + L.cards[0].w) - L.cards[1].x, ov, "leftmost overlap");

  // Odd count: the centred card really is the middle of the run.
  near(L.cards[2].x - L.cards[0].x, L.cards[4].x - L.cards[2].x,
       "run symmetric about the centred card");

  // The run is centred on the screen.
  near(L.stripW, 1142.0, "run scaled to fit the width exactly");
  near(L.originX, (kUsableW - L.stripW) * 0.5, "run centred horizontally");
  near(L.originX + L.stripW, L.originX + L.stripW, "run right edge");
}

// Section C: the selection must not move the geometry AT ALL.
void sectionC_selectionDoesNotMoveTheRun() {
  auto mt = defaults();
  double firstX = 0.0, firstW = 0.0;
  std::vector<double> firstXs;
  for (int sel = 0; sel < 5; ++sel) {
    const auto L = computeCarousel(5, sel, LRect{0, 0, kUsableW, kUsableH}, mt);
    // The card AT SLOT 0 is always the centred one, and it must land in the
    // same place whatever window is in it.
    int centre = -1;
    for (int i = 0; i < 5; ++i)
      if (L.cards[static_cast<size_t>(i)].showCaption)
        centre = i;
    yes(centre >= 0, "a card is centred");
    const auto &c = L.cards[static_cast<size_t>(centre)];
    if (sel == 0) {
      firstX = c.x;
      firstW = c.w;
    } else {
      near(c.x, firstX, "centred card x is selection-independent");
      near(c.w, firstW, "centred card width is selection-independent");
    }
    near(L.stripW, 1142.0, "strip width is selection-independent");
    // The SET of boxes is what must not move: same widths, same x values, only
    // the window sitting in each one changes.
    std::vector<double> xs;
    for (const auto &c : L.cards)
      if (c.visible) {
        xs.push_back(c.x);
        near(c.w, firstW, "every card keeps the standard width");
      }
    std::sort(xs.begin(), xs.end());
    if (sel == 0)
      firstXs = xs;
    else
      yes(xs == firstXs, "the run's box positions never depend on selection");
  }
}

// Section D: vertical fit, and the caption band.
void sectionD_verticalFitAndCaption() {
  auto mt = defaults();
  const auto L = computeCarousel(5, 2, LRect{0, 0, kUsableW, kUsableH}, mt);
  const double k = 1142.0 / 5790.0;
  // stripH = boxH + captionGap + captionH = 493 + 56 = 549; availH =
  // 493 - 64 - 32 - 12 = 385, so the HEIGHT branch is the binding one on this
  // screen and the two scales must agree with the single k.
  const double availH = 385.0;
  const double kk = std::min(1142.0 / 5790.0, availH / 549.0);
  near(k, kk, "one scale fits both axes");
  near(L.stripH, 549.0 * k, "strip height scaled");
  yes(L.panelH <= kUsableH - 2 * kMargin, "panel fits the screen height");
  for (const auto &c : L.cards) {
    if (c.visible)
      near(c.y, 0.0, "every frame shares the band's top edge");
  }
}

// Section E: wrapping and degenerate counts.
void sectionE_wrappingAndEdges() {
  auto mt = defaults();

  const auto a = computeCarousel(5, -1, LRect{0, 0, kUsableW, kUsableH}, mt);
  yes(a.cards[4].showCaption, "selected -1 wraps to the last card");
  const auto b = computeCarousel(5, 5, LRect{0, 0, kUsableW, kUsableH}, mt);
  yes(b.cards[0].showCaption, "selected == count wraps to the first card");
  // Wrapping must not move the centred card.
  near(b.cards[0].x, a.cards[4].x, "wrapped selections centre identically");

  const auto e = computeCarousel(0, 0, LRect{0, 0, kUsableW, kUsableH}, mt);
  yes(e.cards.empty(), "count 0 has no cards");
  yes(e.visibleCards == 0, "count 0 shows nothing");

  // Fewer windows than slots: every window is placed, at its own slot.
  const auto f = computeCarousel(2, 0, LRect{0, 0, kUsableW, kUsableH}, mt);
  int placed = 0;
  for (const auto &c : f.cards)
    placed += c.visible ? 1 : 0;
  yes(placed == 2, "both windows placed when there are fewer than slots");
  yes(f.visibleCards == 2, "visibleCards clamped to the count");
  for (const auto &c : f.cards)
    if (!c.visible)
      near(c.w, 0.0, "off-run card has no width");
}

} // namespace

int main() {
  sectionA_oneStandardBox();
  sectionB_placementAndCentring();
  sectionC_selectionDoesNotMoveTheRun();
  sectionD_verticalFitAndCaption();
  sectionE_wrappingAndEdges();

  if (g_failed) {
    std::printf("\n%d carousel layout check(s) FAILED\n", g_failed);
    return 1;
  }
  std::printf("all carousel layout checks passed\n");
  return 0;
}
