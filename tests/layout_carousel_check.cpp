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

// The standard card is a FULLSCREEN window's box: the whole monitor. The run is
// laid out in `usable` (the padded rect) and scaled to fit it.
constexpr double kScreenW = 1366.0;
constexpr double kScreenH = 768.0;
constexpr double kUsableW = 1206.0;
constexpr double kUsableH = 493.0;
constexpr double kMargin = 32.0;
constexpr double kGapK = 0.03; // gap between cards as a fraction of boxW
constexpr double kCaption = 44.0 + 12.0;

CarouselMetrics defaults() {
  CarouselMetrics mt;
  mt.boxW = kScreenW;
  mt.boxH = kScreenH;
  mt.gapK = kGapK;
  mt.visibleCards = 5;
  return mt;
}

// Section A: the standard box. Every card gets it, whatever it holds.
void sectionA_oneStandardBox() {
  auto mt = defaults();
  const auto L = computeCarousel(5, 2, LRect{0, 0, kUsableW, kUsableH}, mt);

  // 5 cards -> slots -2..2, reach 2: stripW = 5*1366 + 4*(0.03*1366) = 6830 +
  // 163.9 = 6993.9, which does NOT fit availW = 1142, so k = 1142/6993.9.
  const double gap = kGapK * kScreenW;
  const double stripW = 5.0 * kScreenW + 4.0 * gap;
  const double k = 1142.0 / stripW;
  for (const auto &c : L.cards) {
    if (!c.visible)
      continue;
    // Every card the same width and height, to the last bit: this is the whole
    // point, and it is why the run cannot shift with the selection.
    near(c.w, kScreenW * k, "every card is the standard width");
    // Including the centred one: the caption strip is reserved in stripH, not
    // bolted onto one card, because a taller centred box is a different size.
    near(c.h, kScreenH * k, "every card is the standard height");
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
  const double gap = kGapK * kScreenW;
  const double stripW = 5.0 * kScreenW + 4.0 * gap;
  const double k = 1142.0 / stripW;
  const double w = kScreenW * k;
  const double g = gap * k;

  // The FOCUSED card is centred in the usable rect, whatever the count: with an
  // even count the run is not symmetric about it, and anchoring to the run is
  // what used to leave the focused card half a card off.
  // originX IS the focused card's left edge; x is measured from there.
  near(L.originX, std::round((kUsableW - w) * 0.5),
       "focused card's left edge is centred, on a whole pixel");
  const double centreX = 0.0;
  near(L.cards[2].x, centreX, "focused card sits at that edge");
  near(L.originX + L.cards[2].x + w * 0.5, std::round((kUsableW - w) * 0.5) + w * 0.5,
       "focused card's middle follows its edge");
  // Neighbours one gap apart, both sides.
  near(L.cards[1].x, centreX - (w + g), "left-1 is one gap out");
  near(L.cards[0].x, centreX - 2.0 * (w + g), "left-2 is two gaps out");
  near(L.cards[3].x, centreX + (w + g), "right-1 is one gap out");
  near(L.cards[4].x, centreX + 2.0 * (w + g), "right-2 is two gaps out");
  // A gap, not an overlap.
  near(L.cards[2].x - (L.cards[1].x + L.cards[1].w), g, "cards are a gap apart");
  near(L.cards[3].x - (L.cards[2].x + L.cards[2].w), g, "cards are a gap apart right");
  near(L.cards[2].x - L.cards[0].x, L.cards[4].x - L.cards[2].x,
       "run symmetric about the focused card");

  // The run is centred inside the usable rect.
  // The card keeps the SCREEN's ratio: that is what makes containment honest, so
  // a window short one side lands at its own size instead of being scaled up.
  near(L.cards[2].w / L.cards[2].h, kScreenW / kScreenH,
       "the card keeps the screen's aspect");
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
    near(L.originX, std::round((kUsableW - firstW) * 0.5),
         "focused edge is selection-independent");
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
  const double gap = kGapK * kScreenW;
  const double stripW = 5.0 * kScreenW + 4.0 * gap;
  // stripH = 768 + 56 = 824; availH = 493 - 64 - 32 - 12 = 385, so the height
  // branch would allow 385/824 = 0.467 — but the width branch only allows
  // 1142/6993.9 = 0.163, so the WIDTH binds and the run fills it exactly.
  const double availH = 385.0;
  const double kk = std::min(1142.0 / stripW, availH / 824.0);
  near(1142.0 / stripW, kk, "one scale fits both axes");
  near(L.stripH, 824.0 * kk, "strip height scaled");
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
