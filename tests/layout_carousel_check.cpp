// Self-check for the carousel geometry (src/layout.cpp): the numbers are pinned
// against hand-computed values rather than left to "looks right on screen".
// Layout is pure geometry with no compositor dependency, which is what makes
// this possible.
//
//   g++ -std=c++23 -Isrc tests/layout_carousel_check.cpp src/layout.cpp
//   -o /tmp/carousel_check && /tmp/carousel_check
//
// Build with -DGLOVIEW_CHECK_VERBOSE to print the resolved layout instead of
// only asserting.

#include "../src/layout.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace gloview;

namespace {

int g_failed = 0;

void near(double got, double want, const char *what, double tol = 1e-9) {
  if (std::abs(got - want) > tol) {
    std::printf("FAIL %-34s got %.4f want %.4f\n", what, got, want);
    ++g_failed;
  }
#ifdef GLOVIEW_CHECK_VERBOSE
  else {
    std::printf("ok   %-34s %.4f\n", what, got);
  }
#endif
}

void is(int got, int want, const char *what) {
  if (got != want) {
    std::printf("FAIL %-34s got %d want %d\n", what, got, want);
    ++g_failed;
  }
#ifdef GLOVIEW_CHECK_VERBOSE
  else
    std::printf("ok   %-34s %d\n", what, got);
#endif
}

void yes(bool cond, const char *what) {
  if (!cond) {
    std::printf("FAIL %-34s\n", what);
    ++g_failed;
  }
#ifdef GLOVIEW_CHECK_VERBOSE
  else
    std::printf("ok   %-34s true\n", what);
#endif
}

} // namespace

int main() {
  // ---- 1920x1080, 5 cards: the source design's nominal case -------------------
  // Hand-computed with Style's constants (spaceXs 4, spaceMd 12, aspect 1.6,
  // cards 500/380/280, overlap 60, caption 44, count 88x32):
  //   stripWidth  = 500 + (380-60)*2 + (280-60)*2      = 1580
  //   selFrameH   = (500-8)/1.6 + 8                   = 315.5
  //   stripHeight = 315.5 + 12 + 44                   = 371.5
  //   panelHeight = 32 + 12 + 371.5                   = 415.5  (< 1080-64)
  //   panel       = centered: x=170, y=round(332.25)=332
  const CarouselMetrics mt;
  {
    const auto L = computeCarousel(5, 2, LRect{0, 0, 1920, 1080}, mt);
    is(L.visibleCards, 5, "1080p/5: visibleCards");
    near(L.stripW, 1580, "1080p/5: stripWidth");
    near(L.stripH, 371.5, "1080p/5: stripHeight");
    near(L.panelW, 1580, "1080p/5: panelWidth");
    near(L.panelH, 415.5, "1080p/5: panelHeight");
    near(L.originX, 170, "1080p/5: panelX centered");
    near(L.originY, 332, "1080p/5: panelY centered");
    near(L.countX, 746, "1080p/5: counterX");
    near(L.stripY, 44, "1080p/5: stripY (count 32 + gap 12)");

    // Selection 2 of 5 → centerSlot 2, start 0. Slots run far-left → selected.
    is(L.cards[0].depth, 2, "1080p/5: slot0 depth");
    is(L.cards[1].depth, 1, "1080p/5: slot1 depth");
    is(L.cards[2].depth, 0, "1080p/5: slot2 is selected");
    yes(L.cards[2].showCaption, "1080p/5: selected carries the caption");
    yes(!L.cards[1].showCaption, "1080p/5: others have no caption");
    is(L.cards[2].zIndex, 3, "1080p/5: selected zIndex");
    is(L.cards[0].zIndex, 1, "1080p/5: far zIndex");
    is(L.cards[0].direction, -1, "1080p/5: far-left direction");
    is(L.cards[4].direction, 1, "1080p/5: far-right direction");

    // x: selX = (1580-500)/2 = 540; nearL = 540-380+60 = 220;
    //    nearR = 540+500-60 = 980.
    near(L.cards[0].x, 0, "1080p/5: far-left x");
    near(L.cards[1].x, 220, "1080p/5: near-left x");
    near(L.cards[2].x, 540, "1080p/5: selected x");
    near(L.cards[3].x, 980, "1080p/5: near-right x");
    near(L.cards[4].x, 1300, "1080p/5: far-right x");

    // y centers each shorter card's frame against the SELECTED frame
    // (315.5), which is the tall one (it carries the caption below).
    near(L.cards[2].y, 0, "1080p/5: selected y");
    near(L.cards[1].y, 37.5, "1080p/5: near y (240.5 frame)");
    near(L.cards[0].y, 68.75, "1080p/5: far y (178 frame)");

    // Card heights: near/far are frame-only; the selected adds caption+gap.
    near(L.cards[0].h, 178, "1080p/5: far card height");
    near(L.cards[1].h, 240.5, "1080p/5: near card height");
    near(L.cards[2].h, 371.5, "1080p/5: selected card height");

    // Cards OVERLAP by design (that is what makes it a carousel): each card's
    // outer edge is tucked `overlap` px under its inward neighbor's.
    near(L.cards[0].x + L.cards[0].w - L.cards[1].x, mt.overlap,
        "1080p/5: 0 tucks under 1");
    near(L.cards[1].x + L.cards[1].w - L.cards[2].x, mt.overlap,
        "1080p/5: 1 tucks under selected");
    // Right side mirrors: each card's LEFT edge tucks under its inward
    // neighbor's right edge.
    near(L.cards[2].x + L.cards[2].w - L.cards[3].x, mt.overlap,
        "1080p/5: 3 tucks under selected");
    near(L.cards[3].x + L.cards[3].w - L.cards[4].x, mt.overlap,
        "1080p/5: 4 tucks under 3");
    // The outermost cards reach exactly the run's edges — no dead margin.
    near(L.cards[0].x, 0, "1080p/5: run starts flush");
    near(L.cards[4].x + L.cards[4].w, L.stripW, "1080p/5: run ends flush");

    // ---- the landing box: FIXED aspect, whatever the window's own ----------
    // pv is the card inset by frameInset; its aspect is 1.6 regardless of the
    // card's width, which is what gives a 16:10 and a 9:21 window the same
    // card. The window over-covers it, so no margin shows inside the ring.
    for (int i = 0; i < 5; ++i) {
      const auto pv = carouselPreviewBox(
          LRect{L.cards[i].x, L.cards[i].y, L.cards[i].w, L.cards[i].h}, mt);
      near(pv.w / pv.h, 1.6, "preview box aspect is fixed");
      near(pv.x, L.cards[i].x + mt.frameInset, "preview box inset x");
      near(pv.y, L.cards[i].y + mt.frameInset, "preview box inset y");
    }
  }

  // ---- wrapping: selection is circular, and the run wraps with it --------
  {
    const auto L = computeCarousel(5, 4, LRect{0, 0, 1920, 1080}, mt);
    // start = (4 - 2) mod 5 = 2 → tile 4 lands in the center slot.
    is(L.cards[4].depth, 0, "wrap: last tile centers at selection 4");
    // start = (4-2) mod 5 = 2, so the run reads 2,3,[4],0,1 — tile 0 comes
    // in on the RIGHT of the centered tile.
    is(L.cards[0].depth, 1, "wrap: tile 0 is near-right");
    is(L.cards[1].depth, 2, "wrap: tile 1 is far-right");
    is(L.cards[3].depth, 1, "wrap: tile 3 is near-left");
    yes(L.cards[0].visible, "wrap: tile 0 still visible");
    // Negative selection must not index out of range.
    const auto N = computeCarousel(5, -1, LRect{0, 0, 1920, 1080}, mt);
    is(N.cards[4].depth, 0, "wrap: negative selection wraps to last");
  }

  // ---- off-run cards are marked invisible, never given a slot -----------
  {
    const auto L = computeCarousel(9, 4, LRect{0, 0, 1920, 1080}, mt);
    int visible = 0;
    for (const auto &c : L.cards) {
      if (c.visible)
        ++visible;
      else
        yes(c.w == 0 && c.h == 0, "off-run card has no slot");
    }
    is(visible, 5, "9 cards: only 5 in the run");
    yes(!L.cards[0].visible, "9 cards: tile 0 is out of the run");
    yes(L.cards[4].visible, "9 cards: the selected tile is in the run");
  }

  // ---- narrow screen: the whole run scales down, card count is NOT shed ---
  {
    // The source design sheds slots down to 3 when the screen is tight
    // (windowSwitcherNarrowLayoutThreshold). We don't: the configured card
    // count is the user's explicit choice, so the run scales uniformly to fit
    // instead — a 5-card carousel on a narrow screen is 5 smaller cards, not 3
    // cards with 2 windows silently missing.
    CarouselMetrics m2 = mt;
    m2.visibleCards = 5;
    const auto L = computeCarousel(9, 4, LRect{0, 0, 1000, 1080}, m2);
    is(L.visibleCards, 5, "narrow: keeps the configured 5 cards");
    // start = (4-2) mod 9 = 2 → tiles 2..6 are in the run.
    yes(L.cards[2].visible && L.cards[6].visible,
        "narrow: the five run cards are visible");
    yes(!L.cards[1].visible && !L.cards[7].visible,
        "narrow: cards outside the run are not");
    is(L.cards[4].depth, 0, "narrow: selected is tile 4");
    yes(L.stripW <= 1000.0 - 2 * mt.screenMargin + 1e-9,
        "narrow: run fits inside the margins");
    // Depth ratios survive the uniform scale: 500 : 380 : 280.
    near(L.cards[4].w / L.cards[3].w, 500.0 / 380.0,
         "narrow: sel/near ratio kept");
    near(L.cards[3].w / L.cards[2].w, 380.0 / 280.0,
         "narrow: near/far ratio kept");
  }

  // ---- the shrink is MONOTONIC at every depth ----------------------------
  // Regression: the source design has widths only for depths 0/1/2 and reuses farW
  // beyond, so a 7-card run ended in two identically sized cards. Every card
  // out from the center must be strictly smaller than the one inside it.
  for (const int n : {2, 3, 4, 5, 6, 7, 9, 12, 20}) {
    for (const int sel : {0, 1, 5, -1}) {
      CarouselMetrics m2 = mt;
      m2.visibleCards = n;
      // Wide/tall enough that no uniform down-scale kicks in, so the widths can
      // be compared against the nominal ladder directly.
      const auto L = computeCarousel(n, sel, LRect{0, 0, 20000, 12000}, m2);
      const int shown = std::min(n, m2.visibleCards);
      const int centerSlot = shown / 2;
      for (const int side : {-1, 1}) {
        const int slots = side < 0 ? centerSlot : shown - 1 - centerSlot;
        double prev = mt.selectedW;
        for (int d = 1; d <= slots; ++d) {
          const int idx = ((sel + side * d) % n + n) % n;
          const auto &c = L.cards[static_cast<size_t>(idx)];
          yes(c.visible, "monotonic: card in the run is visible");
          is(c.depth, d, "monotonic: depth matches the step");
          // Reference widths for depths 0..2, then the geometric taper.
          const double want = d <= 1 ? (d == 1 ? mt.nearW : mt.selectedW)
                             : mt.farW * std::pow(mt.taper, d - 2.0);
          near(c.w, want, "monotonic: width matches the ladder");
          if (!(c.w < prev - 1e-9)) {
            std::printf("FAIL monotonic n=%d sel=%d side=%d depth=%d: %.3f "
                        "not smaller than %.3f\n",
                        n, sel, side, d, c.w, prev);
            ++g_failed;
          }
          prev = c.w;
        }
      }
    }
  }

  // ---- even card counts put the selection one slot off center -------------
  {
    CarouselMetrics m2 = mt;
    m2.visibleCards = 4;
    const auto L = computeCarousel(8, 3, LRect{0, 0, 1920, 1080}, m2);
    is(L.visibleCards, 4, "even: four cards shown");
    is(L.cards[3].depth, 0, "even: selected is centered");
    is(L.cards[2].depth, 1, "even: one card to its left");
    is(L.cards[1].depth, 2, "even: two cards to its left");
    is(L.cards[4].depth, 1, "even: one card to its right");
    yes(!L.cards[5].visible, "even: the 5th card is out of the run");
  }

  // ---- 6 and 12 cards: exactly the configured count, nothing dropped -----
  {
    for (const int n : {3, 6, 9, 12}) {
      CarouselMetrics m2 = mt;
      m2.visibleCards = n;
      const auto L = computeCarousel(20, 0, LRect{0, 0, 2560, 1440}, m2);
      is(L.visibleCards, n, "count: shows exactly the configured n");
      int visible = 0;
      for (const auto &c : L.cards)
        if (c.visible)
          ++visible;
      is(visible, n, "count: exactly n cards marked visible");
      // And it still fits.
      yes(L.stripW <= 2560.0 - 2 * mt.screenMargin + 1e-9,
          "count: run fits the screen");
    }
  }

  // ---- card width is the user's knob -------------------------------------
  {
    CarouselMetrics wide = mt;
    wide.selectedW = 700.0;
    wide.nearW = 700.0 * 380.0 / 500.0;
    wide.farW = 700.0 * 280.0 / 500.0;
    const auto W = computeCarousel(5, 2, LRect{0, 0, 2560, 1440}, wide);
    near(W.cards[2].w, 700.0, "size: selected honors selectedW");
    near(W.cards[1].w, 700.0 * 380.0 / 500.0, "size: near scales with it");
    near(W.cards[0].w, 700.0 * 280.0 / 500.0, "size: far scales with it");
  }

  // ---- short screen: the two-pass height fit shrinks the cards -----------
  {
    // 1920x400 → maxPanelHeight 336, well under the nominal 415.5.
    const auto L = computeCarousel(5, 2, LRect{0, 0, 1920, 400}, mt);
    is(L.visibleCards, 5, "short: width is ample, no slots shed");
    yes(L.panelH < 415.5, "short: panel shrank");
    yes(L.cards[2].w < 500, "short: selected card shrank");
    yes(L.cards[3].w < 380, "short: near card shrank");
    // The fit is a fixed TWO passes (the source design's kHeightFitPasses), and it
    // converges geometrically rather than exactly: shrinking the widths also
    // shrinks the fixed frameInset/caption constants' share, so the residual
    // after pass 2 is a couple of px. Assert it lands CLOSE — not <= — which
    // is what the source design actually delivers.
    yes(L.panelH <= 336.0 * 1.02, "short: panel within 2% of the limit");
    // Aspect is preserved by the fit (it scales widths, frameH follows).
    const auto pv = carouselPreviewBox(
        LRect{L.cards[2].x, L.cards[2].y, L.cards[2].w, L.cards[2].h}, mt);
    near(pv.w / pv.h, 1.6, "short: preview aspect still fixed");
  }

  // ---- fewer cards than slots: centerSlot follows the actual count ------
  {
    // 2 cards → visibleCount 2, centerSlot 1, start = (sel-1) mod 2.
    const auto L = computeCarousel(2, 0, LRect{0, 0, 1920, 1080}, mt);
    is(L.cards[0].depth, 0, "2 cards: selected centered");
    is(L.cards[1].depth, 1, "2 cards: other is near");
    yes(!L.cards[1].showCaption, "2 cards: only selected captioned");
  }

  // ---- degenerate: no windows ------------------------------------------
  {
    const auto L = computeCarousel(0, 0, LRect{0, 0, 1920, 1080}, mt);
    yes(L.cards.empty(), "empty: no cards");
    near(L.panelW, 0, "empty: no panel");
  }

  if (g_failed) {
    std::printf("\n%d check(s) FAILED\n", g_failed);
    return 1;
  }
  std::printf("all carousel layout checks passed\n");
  return 0;
}
