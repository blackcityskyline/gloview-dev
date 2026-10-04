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

#include "layout.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace gloview;

namespace {

int g_failed = 0;

void near(double got, double want, const char *what, double tol = 1e-9) {
  if (std::abs(got - want) > tol) {
    std::printf("FAIL %-42s got %.6f want %.6f\n", what, got, want);
    ++g_failed;
  }
}

void yes(bool cond, const char *what) {
  if (!cond) {
    std::printf("FAIL %s\n", what);
    ++g_failed;
  }
}

// 16:9 — the aspect a window falls back to when it reports none.
constexpr double k169 = 16.0 / 9.0;

std::vector<double> uniformAspect(int n, double a) {
  return std::vector<double>(static_cast<size_t>(n), a);
}

} // namespace

int main() {
  const CarouselMetrics mt; // selectedW 500, nearW 380, farW 280, taper 0.8,
                             // overlap 60, captionGap 12, captionH 44,
                             // countH 32, sectionGap 12, screenMargin 32,
                             // visibleCards 5

  // ---- A. width ladder, 1920x1080, 5 cards, selection 2 ---------------------
  // Slots: shown 5 -> centerSlot 5/2 = 2, leftSlots 2, rightSlots 5-1-2 = 2.
  // nominal = selectedW + (nearW-overlap)*2 + (farW-overlap)*2
  //         = 500 + (380-60)*2 + (280-60)*2 = 500 + 640 + 440 = 1580.
  // availableW = 1920 - 2*32 = 1856 > 1580, so wScale = 1: no width shrink.
  // stripW    = 1580, and the selected card sits centred in it:
  //   selX = (1580 - 500)/2 = 540.
  // A side card steps to the OUTWARD card's width and then tucks `overlap`
  // back under its inward neighbour:
  //   slot -1 = 540 - 380 + 60 = 220      slot +1 = 540 + 500 - 60 = 980
  //   slot -2 = 220 - 280 + 60 = 0        slot +2 = 980 + 380 - 60 = 1300
  // Outermost edges are flush with the run: 0 on the left, and
  // 1300 + 280 = 1580 = stripW on the right.
  //
  // Height is the WINDOW's own shape (all windows 16:9 here):
  //   depth0 (selected) 500/(16/9) = 500*9/16 = 281.25, +captionGap+captionH
  //          = 281.25 + 12 + 44 = 337.25   <- the tallest, so stripH = 337.25
  //   depth1            380*9/16 = 213.75
  //   depth2            280*9/16 = 157.5
  // Vertical fit: avail = 1080 - 2*32 - countH - sectionGap
  //              = 1016 - 32 - 12 = 972 >= 337.25, so hScale = 1.
  {
    const auto L = computeCarousel(5, 2, LRect{0, 0, 1920, 1080},
                                   uniformAspect(5, k169), mt);
    near(static_cast<double>(L.visibleCards), 5, "A: five cards in the run");
    near(L.stripW, 1580, "A: stripW = 500 + 2*(380-60) + 2*(280-60)");
    near(L.stripH, 337.25, "A: stripH = tallest card, the selected one");

    near(L.cards[2].x, 540, "A: selected x = (1580-500)/2");
    near(L.cards[1].x, 220, "A: near-left x = 540-380+60");
    near(L.cards[0].x, 0, "A: far-left x = 220-280+60 (flush left edge)");
    near(L.cards[3].x, 980, "A: near-right x = 540+500-60");
    near(L.cards[4].x, 1300, "A: far-right x = 980+380-60");
    near(L.cards[4].x + L.cards[4].w, L.stripW, "A: run ends flush");

    near(L.cards[2].w, 500, "A: selected width");
    near(L.cards[1].w, 380, "A: depth1 width");
    near(L.cards[3].w, 380, "A: depth1 width (mirrored)");
    near(L.cards[0].w, 280, "A: depth2 width");
    near(L.cards[4].w, 280, "A: depth2 width (mirrored)");

    near(L.cards[2].h, 337.25, "A: selected height = 281.25 + caption band");
    near(L.cards[1].h, 213.75, "A: depth1 height = 380*9/16");
    near(L.cards[3].h, 213.75, "A: depth1 height (mirrored)");
    near(L.cards[0].h, 157.5, "A: depth2 height = 280*9/16");
    near(L.cards[4].h, 157.5, "A: depth2 height (mirrored)");

    // Widths shrink outward; the selected card carries the caption alone.
    yes(L.cards[2].w > L.cards[1].w, "A: depth0 wider than depth1");
    yes(L.cards[1].w > L.cards[0].w, "A: depth1 wider than depth2");
    yes(L.cards[2].showCaption, "A: selected card shows the caption");
    yes(!L.cards[1].showCaption && !L.cards[0].showCaption,
        "A: side cards show no caption");
  }

  // ---- the taper past depth 2, 9 cards in the run --------------------------
  // Beyond depth 2 the width keeps tapering by `taper` per step, so no two
  // cards out from the center are the same size:
  //   depth0 500, depth1 380, depth2 280*0.8^0 = 280,
  //   depth3 280*0.8^1 = 224, depth4 280*0.8^2 = 179.2.
  // nominal = 500 + 2*((380-60)+(280-60)+(224-60)+(179.2-60))
  //         = 500 + 2*(320+220+164+119.2) = 500 + 2*823.2 = 2146.4.
  // availableW = 4000 - 64 = 3936 > 2146.4 -> wScale = 1.
  // stripW = 2146.4 -> selX = (2146.4-500)/2 = 823.2.
  // Heights: 500*9/16+56 = 337.25, 380*9/16 = 213.75, 280*9/16 = 157.5,
  //   224*9/16 = 126, 179.2*9/16 = 100.8. stripH = 337.25 again.
  // Vertical fit: avail = 3000 - 64 - 44 = 2892 >= 337.25 -> hScale = 1.
  {
    CarouselMetrics m9 = mt;
    m9.visibleCards = 9;
    const auto L = computeCarousel(9, 4, LRect{0, 0, 4000, 3000},
                                   uniformAspect(9, k169), m9);
    near(L.stripW, 2146.4, "ladder: stripW of a 9-slot run");
    near(L.cards[4].x, 823.2, "ladder: selected x = (2146.4-500)/2");

    // depth d lives at index sel-d on the left, sel+d on the right.
    const double wantW[5] = {500.0, 380.0, 280.0, 224.0, 179.2};
    const double wantH[5] = {337.25, 213.75, 157.5, 126.0, 100.8};
    for (int d = 0; d < 5; ++d) {
      for (const int side : {-1, 1}) {
        const int idx = 4 + side * d;
        const auto &c = L.cards[static_cast<size_t>(idx)];
        near(c.w, wantW[d], "ladder: width by depth");
        near(c.h, wantH[d], "ladder: height by depth");
      }
      // Compare depth d against depth d-1 once per depth (both sides of a
      // depth are the same size, so the sides are not comparable to each
      // other): width strictly shrinks, and the shorter card starts lower.
      if (d > 0) {
        yes(L.cards[4].w > L.cards[static_cast<size_t>(4 - d)].w,
            "ladder: width shrinks strictly outward");
        yes(L.cards[static_cast<size_t>(4 - d)].y >
                L.cards[static_cast<size_t>(4 - (d - 1))].y,
            "ladder: y descends down the stack");
      }
    }

    // Outermost cards reach the run's edges: left 0, right 1967.2 + 179.2.
    near(L.cards[0].x, 0, "ladder: left end flush");
    near(L.cards[8].x + L.cards[8].w, L.stripW, "ladder: right end flush");
  }

  // ---- B. the card carries the WINDOW's aspect -----------------------------
  // Same ladder on a screen tall enough that nothing scales (1600 high ->
  // avail = 1600 - 64 - 44 = 1492, well above the tallest card). Run 1: every
  // window 16:9, so the selected card is 281.25 + 56 = 337.25 tall. Run 2
  // replaces ONLY the selected window with a 9:21 phone, so its card must be
  //   500/(9/21) = 500*21/9 = 1166.6667, +56 = 1222.6667 tall —
  // at the SAME width (the width ladder is a pure function of depth), and at
  // the same x (selX depends on widths only). Side windows keep 16:9, so their
  // heights are untouched and they re-centre against the taller frame:
  //   (1222.6667 - 213.75)/2 = 504.4583.
  {
    const auto L1 = computeCarousel(5, 2, LRect{0, 0, 1920, 1600},
                                    uniformAspect(5, k169), mt);
    auto a = uniformAspect(5, k169);
    a[2] = 9.0 / 21.0;
    const auto L2 = computeCarousel(5, 2, LRect{0, 0, 1920, 1600}, a, mt);

    near(L2.cards[2].w, L1.cards[2].w, "B: 9:21 keeps the selected width");
    near(L2.cards[2].x, L1.cards[2].x, "B: 9:21 keeps the selected x");
    near(L2.cards[2].w, 500, "B: selected width is still 500");
    near(L2.cards[2].x, 540, "B: selected x is still (1580-500)/2");
    near(L2.cards[2].h, 500.0 * 21.0 / 9.0 + 56.0, "B: 9:21 selected height");
    yes(L2.cards[2].h > L1.cards[2].h, "B: 9:21 selected card is taller");
    near(L2.stripH, 500.0 * 21.0 / 9.0 + 56.0, "B: stripH follows the tall card");

    // Side cards: same own-aspect heights as run 1, only re-centred.
    near(L2.cards[1].h, L1.cards[1].h, "B: side height unchanged");
    near(L2.cards[1].y, (L2.cards[2].h - L2.cards[1].h) / 2.0,
         "B: side card re-centres on the taller selected frame");
  }

  // ---- C. the run always fits the screen height ----------------------------
  // Short screen: avail = 400 - 2*32 - 32 - 12 = 292 < 337.25, so the whole
  // run scales by hScale = 292/337.25 = 0.865827... and the panel lands
  // exactly on the limit: stripH*hScale + countH + sectionGap = 292 + 44 = 336
  // = screen.h - 2*screenMargin.
  // Tall screen: avail = 972 > 337.25 -> hScale = 1, nothing shrinks.
  {
    const auto S = computeCarousel(5, 2, LRect{0, 0, 1920, 400},
                                   uniformAspect(5, k169), mt);
    near(S.panelH, 336.0, "C: short screen: panelH lands on the limit");
    yes(S.panelH <= 400.0 - 2.0 * mt.screenMargin + 1e-9,
        "C: short screen: run fits inside the margins");
    near(S.stripH, 292.0, "C: short screen: stripH = avail after chrome", 1e-9);
    yes(S.cards[2].w < 500.0, "C: short screen: selected card shrank");
    // A uniform shrink: the depth ratios are untouched.
    near(S.cards[2].w / S.cards[1].w, 500.0 / 380.0,
         "C: short screen: selected/near ratio kept");
    near(S.cards[1].w / S.cards[0].w, 380.0 / 280.0,
         "C: short screen: near/far ratio kept");

    const auto T = computeCarousel(5, 2, LRect{0, 0, 1920, 1080},
                                   uniformAspect(5, k169), mt);
    near(T.cards[2].w, 500.0, "C: tall screen: hScale is 1 (width untouched)");
    near(T.stripH, 337.25, "C: tall screen: hScale is 1 (height untouched)");
    near(T.cards[1].h, 213.75, "C: tall screen: side height unscaled");
  }

  // ---- D. the selection wraps in both directions ---------------------------
  // sel = ((selected % count) + count) % count: -1 of 5 lands on 4, count of 5
  // lands on 0. Either way the selected card is the depth-0 one and the only
  // captioned one, and its neighbours follow by the same ±1 wrap.
  {
    const auto N = computeCarousel(5, -1, LRect{0, 0, 1920, 1080},
                                   uniformAspect(5, k169), mt);
    near(static_cast<double>(N.cards[4].depth), 0, "D: selection -1 wraps to the last card");
    yes(N.cards[4].showCaption, "D: wrapped selection is captioned");
    near(static_cast<double>(N.cards[3].depth), 1, "D: card before it is depth1");
    near(static_cast<double>(N.cards[0].depth), 1, "D: wrapped card 0 is depth1 on the right");

    const auto O = computeCarousel(5, 5, LRect{0, 0, 1920, 1080},
                                   uniformAspect(5, k169), mt);
    near(static_cast<double>(O.cards[0].depth), 0, "D: selection == count wraps to 0");
    yes(O.cards[0].showCaption, "D: wrapped selection 0 is captioned");
    near(static_cast<double>(O.cards[4].depth), 1, "D: card before it is depth1");
    for (const auto &c : O.cards)
      yes(c.depth != 0 || c.showCaption, "D: depth0 implies the caption");

    // No windows: nothing to lay out, and no crash.
    const auto Z = computeCarousel(0, 0, LRect{0, 0, 1920, 1080}, {}, mt);
    yes(Z.cards.empty(), "D: count 0 yields no cards");
    near(static_cast<double>(Z.visibleCards), 0, "D: count 0 yields no visible cards");
    near(Z.panelW, 0, "D: count 0 yields no panel");
    near(Z.stripH, 0, "D: count 0 yields no strip");
  }

  // ---- E. more windows than slots: only the run is laid out ----------------
  // count 9, visibleCards 5, selection 4 -> slots -2..2 map to indices
  // 2,3,4,5,6. The other four stay default-constructed: not visible, zero box.
  {
    const auto L = computeCarousel(9, 4, LRect{0, 0, 1920, 1080},
                                   uniformAspect(9, k169), mt);
    near(static_cast<double>(L.visibleCards), 5, "E: still five cards");
    int visible = 0;
    for (const auto &c : L.cards) {
      if (c.visible)
        ++visible;
      else
        yes(c.w == 0.0 && c.h == 0.0 && c.x == 0.0 && c.y == 0.0,
            "E: card outside the run keeps a zero box");
    }
    near(static_cast<double>(visible), 5, "E: exactly five cards marked visible");
    for (const int i : {2, 3, 4, 5, 6})
      yes(L.cards[static_cast<size_t>(i)].visible, "E: run member is visible");
    for (const int i : {0, 1, 7, 8})
      yes(!L.cards[static_cast<size_t>(i)].visible, "E: non-member is not visible");
    near(static_cast<double>(L.cards[4].depth), 0, "E: the selected card is in the run");
  }

  // ---- F. side cards centre on the selected frame --------------------------
  // Every visible side card is offset by exactly half the height difference
  // against the SELECTED card's box — the selected card is the tall one
  // because it alone carries the caption band.
  //   depth1: (337.25 - 213.75)/2 = 61.75
  //   depth2: (337.25 - 157.5)/2  = 89.875
  // The selected card itself sits at 0 (it is the reference box).
  {
    const auto L = computeCarousel(5, 2, LRect{0, 0, 1920, 1080},
                                   uniformAspect(5, k169), mt);
    near(L.cards[1].y, 61.75, "F: near-left y = (337.25-213.75)/2");
    near(L.cards[3].y, 61.75, "F: near-right y = (337.25-213.75)/2");
    near(L.cards[0].y, 89.875, "F: far-left y = (337.25-157.5)/2");
    near(L.cards[4].y, 89.875, "F: far-right y = (337.25-157.5)/2");
    near(L.cards[2].y, 0.0, "F: selected card is the reference box");

    // Same rule after the short-screen shrink, where every height moved.
    const auto S = computeCarousel(5, 2, LRect{0, 0, 1920, 400},
                                   uniformAspect(5, k169), mt);
    for (const int i : {0, 1, 3, 4})
      near(S.cards[static_cast<size_t>(i)].y,
           (S.cards[2].h - S.cards[static_cast<size_t>(i)].h) / 2.0,
           "F: scaled run still centres on the selected box");

    // And it holds for every depth of a long run, not just the first two.
    CarouselMetrics m9 = mt;
    m9.visibleCards = 9;
    const auto D = computeCarousel(9, 4, LRect{0, 0, 4000, 3000},
                                   uniformAspect(9, k169), m9);
    for (int d = 1; d <= 4; ++d)
      for (const int side : {-1, 1}) {
        const auto &c = D.cards[static_cast<size_t>(4 + side * d)];
        near(c.y, (D.cards[4].h - c.h) / 2.0, "F: deep side card centres too");
      }
  }

  if (g_failed) {
    std::printf("\n%d carousel layout check(s) FAILED\n", g_failed);
    return 1;
  }
  std::printf("all carousel layout checks passed\n");
  return 0;
}