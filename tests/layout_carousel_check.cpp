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
// The model, all of it pinned below:
//   * the standard box is a FULLSCREEN window's box — the whole monitor — so a
//     window one side short of the screen lands at its own size inside it;
//   * the focused card IS the standard box; each step out is a fraction of it
//     (1, nearK, farK, then taper), so the run tapers and no card changes its
//     proportions;
//   * cards sit on a CONSTANT pitch, one gap apart, so the taper does not change
//     the spacing;
//   * x is absolute inside the usable rect and the focused card's middle is its
//     centre — the usable rect already carries the strip's band in the padding,
//     so that is the centre of the free space and it moves with the strip;
//   * one scale fits both axes, multiplied by the user's zoom.

#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace gloview;

namespace {

int g_failed = 0;

void near(double got, double want, const char *what, double tol = 1e-9) {
  if (std::abs(got - want) > tol) {
    std::printf("FAIL %-50s got %.9f want %.9f\n", what, got, want);
    ++g_failed;
  }
}

void yes(bool cond, const char *what) {
  if (!cond) {
    std::printf("FAIL %s\n", what);
    ++g_failed;
  }
}

constexpr double kScreenW = 1366.0; // the standard box: a fullscreen window
constexpr double kScreenH = 768.0;
constexpr double kUsableW = 1206.0; // screen minus the layout's left/right padding
constexpr double kUsableH = 493.0;  // screen minus top/bottom padding and strip
constexpr double kMargin = 32.0;
constexpr double kGapK = 0.03;
constexpr double kNearK = 0.76;
constexpr double kFarK = 0.56;
constexpr double kTaper = 0.8;
constexpr double kCaption = 44.0 + 12.0;

CarouselMetrics defaults() {
  CarouselMetrics mt;
  mt.boxW = kScreenW;
  mt.boxH = kScreenH;
  mt.gapK = kGapK;
  mt.nearK = kNearK;
  mt.farK = kFarK;
  mt.taper = kTaper;
  mt.visibleCards = 5;
  return mt;
}

double sizeAt(int depth) {
  if (depth <= 0)
    return 1.0;
  if (depth == 1)
    return kNearK;
  return kFarK * std::pow(kTaper, static_cast<double>(depth - 2));
}

// The scale the layout must arrive at on this screen.
double fittedScale(const CarouselMetrics &mt) {
  const double gap = mt.gapK * mt.boxW;
  const double pitch = mt.boxW + gap;
  const int reach = 2; // 5 cards -> slots -2..2
  const double stripW = mt.boxW + pitch * 2.0 * reach;
  const double stripH = mt.boxH + mt.captionGap + mt.captionH;
  const double availW = kUsableW - 2.0 * mt.screenMargin;
  const double availH = kUsableH - 2.0 * mt.screenMargin - mt.countH -
                        mt.sectionGap;
  return std::min({1.0, availW / stripW, availH / stripH}) * mt.scale;
}

// Section A: the taper. Proportions never change; only the size does.
void sectionA_taper() {
  const auto mt = defaults();
  const double k = fittedScale(mt);
  const auto L = computeCarousel(9, 4, LRect{0, 0, kUsableW, kUsableH}, mt);

  for (int i = 2; i <= 6; ++i) {
    const auto &c = L.cards[static_cast<size_t>(i)];
    const int d = std::abs(i - 4);
    near(c.w / (kScreenW * k), sizeAt(d), "width tapers by depth");
    near(c.h / (kScreenH * k), sizeAt(d), "height tapers with it");
    yes(std::abs(c.w / c.h - kScreenW / kScreenH) < 1e-9,
        "a tapered card keeps the screen's proportions");
    near(c.y, 0.0, "every card shares the band's top edge");
  }
  // Strictly shrinking outward.
  for (int d = 0; d < 4; ++d)
    yes(sizeAt(d) > sizeAt(d + 1), "the taper shrinks strictly");
}

// Section B: the focused card is on the free space's centre; the pitch is fixed.
void sectionB_placementAndCentring() {
  const auto mt = defaults();
  const double k = fittedScale(mt);
  const auto L = computeCarousel(5, 2, LRect{0, 0, kUsableW, kUsableH}, mt);
  const double pitch = (kScreenW + kGapK * kScreenW) * k;

  // 5 cards, sel 2 -> indices 0..4 at slots -2..+2.
  for (int s = -2; s <= 2; ++s) {
    const int idx = (2 + s + 5) % 5;
    const auto &c = L.cards[static_cast<size_t>(idx)];
    // Each card centred one pitch per step from the free space's centre.
    near(c.x + c.w * 0.5, kUsableW * 0.5 + s * pitch,
         "card centre is one pitch per step from the middle");
  }
  // The focused card specifically.
  const auto &f = L.cards[2];
  near(f.x + f.w * 0.5, kUsableW * 0.5, "focused card is dead centre");
  near(L.originX, 0.0, "no separate centring offset is added on top");

  // A gap, never an overlap. The pitch is constant and the outer card is
  // SMALLER, so the clear space is the nominal gap plus half the size
  // difference — which is exactly what a fixed pitch with a taper must give.
  const auto &l = L.cards[1];
  const double nominal = kGapK * kScreenW * k;
  near(f.x - (l.x + l.w), nominal + (f.w - l.w) * 0.5,
       "a smaller neighbour leaves the nominal gap plus half the difference");
  yes(f.x > l.x + l.w, "cards never overlap");

  // The run fits the free space.
  yes(L.stripW * k <= kUsableW - 2.0 * kMargin + 1e-9, "run fits the width");
  yes(L.panelH <= kUsableH - 2.0 * kMargin + 1e-9, "panel fits the height");
  // The counter rides the same centre.
  near(L.countX + L.originX + L.countW * 0.5, kUsableW * 0.5,
       "counter centred in the free space");
}

// Section C: the user's zoom rides on top of the fit.
void sectionC_userZoom() {
  auto mt = defaults();
  const double fit = fittedScale(mt);
  mt.scale = 1.2;
  const auto L = computeCarousel(5, 2, LRect{0, 0, kUsableW, kUsableH}, mt);
  near(fittedScale(mt), fit * 1.2, "the zoom multiplies the fitted scale");
  const auto &f = L.cards[2];
  near(f.w, kScreenW * fit * 1.2, "the focused card is 20% bigger");
  near(f.x + f.w * 0.5, kUsableW * 0.5, "and still centred");

  mt.scale = 0.5;
  const auto S = computeCarousel(5, 2, LRect{0, 0, kUsableW, kUsableH}, mt);
  near(S.cards[2].w, kScreenW * fit * 0.5, "a zoom below 1 shrinks the run");
}

// Section D: selection must not move the geometry AT ALL.
void sectionD_selectionDoesNotMoveTheRun() {
  const auto mt = defaults();
  std::vector<double> xs, ws;
  double firstStrip = 0.0;
  for (int sel = 0; sel < 5; ++sel) {
    const auto L = computeCarousel(5, sel, LRect{0, 0, kUsableW, kUsableH}, mt);
    std::vector<double> sx, sw;
    for (const auto &c : L.cards)
      if (c.visible) {
        sx.push_back(c.x);
        sw.push_back(c.w);
      }
    std::sort(sx.begin(), sx.end());
    std::sort(sw.begin(), sw.end());
    if (sel == 0) {
      xs = sx;
      ws = sw;
      firstStrip = L.stripW;
    } else {
      yes(sx == xs, "box positions never depend on the selection");
      yes(sw == ws, "box sizes never depend on the selection");
      near(L.stripW, firstStrip, "strip width never depends on the selection");
    }
    // Exactly one centred card, at the middle.
    int centred = 0;
    for (const auto &c : L.cards)
      if (c.showCaption)
        ++centred;
    yes(centred == 1, "exactly one card is centred");
  }
}

// Section E: wrapping and degenerate counts.
void sectionE_wrappingAndEdges() {
  const auto mt = defaults();
  const auto a = computeCarousel(5, -1, LRect{0, 0, kUsableW, kUsableH}, mt);
  yes(a.cards[4].showCaption, "selected -1 wraps to the last card");
  const auto b = computeCarousel(5, 5, LRect{0, 0, kUsableW, kUsableH}, mt);
  yes(b.cards[0].showCaption, "selected == count wraps to the first card");
  near(b.cards[0].x, a.cards[4].x, "a wrapped selection centres identically");

  const auto e = computeCarousel(0, 0, LRect{0, 0, kUsableW, kUsableH}, mt);
  yes(e.cards.empty(), "count 0 has no cards");
  yes(e.visibleCards == 0, "count 0 shows nothing");

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
  sectionA_taper();
  sectionB_placementAndCentring();
  sectionC_userZoom();
  sectionD_selectionDoesNotMoveTheRun();
  sectionE_wrappingAndEdges();

  if (g_failed) {
    std::printf("\n%d carousel layout check(s) FAILED\n", g_failed);
    return 1;
  }
  std::printf("all carousel layout checks passed\n");
  return 0;
}
