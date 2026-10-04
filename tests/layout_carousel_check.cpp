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

using namespace gloview;

namespace {

int g_failed = 0;

void near(double got, double want, const char *what, double tol = 1e-9) {
  if (std::abs(got - want) > tol) {
    std::printf("FAIL %-46s got %.9f want %.9f\n", what, got, want);
    ++g_failed;
  }
}

void yes(bool cond, const char *what) {
  if (!cond) {
    std::printf("FAIL %s\n", what);
    ++g_failed;
  }
}

// The screen rect the mode hands to the layout is ALREADY inset (panel padding
// is not the layout's business), so computeCarousel applies its own
// screenMargin inside it. 1206x493 is that inset rect for a 16:9 desktop.
constexpr double kScreenW = 1206.0;
constexpr double kScreenH = 493.0;

} // namespace

int main() {
  const CarouselMetrics mt; // selectedW 500, nearW 380, farW 280, taper 0.8,
                             // overlap 60, aspect 1.6, captionGap 12,
                             // captionH 44, countH 32, sectionGap 12,
                             // screenMargin 32, visibleCards 5
  const LRect screen{0.0, 0.0, kScreenW, kScreenH};

  // ---- A. width ladder, 5 cards, selection 2 -------------------------------
  // Slots: shown 5 -> centerSlot 5/2 = 2, leftSlots 2, rightSlots 5-1-2 = 2.
  // nominal = selectedW + (nearW-overlap)*2 + (farW-overlap)*2
  //         = 500 + (380-60)*2 + (280-60)*2 = 500 + 640 + 440 = 1580.
  // availableW = screen.w - 2*screenMargin = 1206 - 64 = 1142 < 1580, so the
  // run is scaled by wScale = 1142/1580 = 0.7227848101265822 (NOT capped at 1:
  // the run is allowed to shrink to fit the width). Every nominal length is
  // that ratio of its design value:
  //   width(0) = 500*s = 361.3924050632911
  //   width(1) = 380*s = 274.65822784810126
  //   width(2) = 280*s = 202.37974683544303
  //   overlap   =  60*s =  43.36708860759494
  //   stripW    = 1580*s = 1142 exactly == availableW (the run fills the width)
  // and the selected card is centred in it:
  //   selX = (1142 - 500*s)/2 = 540*s = 390.30379746835445.
  // A side card steps out to the OUTWARD card's width, then tucks `overlap`
  // back under its inward neighbour — in nominal px, then scaled:
  //   slot -1 = (540-380+60)*s = 220*s = 159.01265822784814
  //   slot -2 = (220-280+60)*s =   0*s = 0            <- flush left edge
  //   slot +1 = (540+500-60)*s = 980*s = 708.3291139240506
  //   slot +2 = (980+380-60)*s = 1300*s = 939.620253164557
  //   right edge = 1300*s + 280*s = 1580*s = 1142 = stripW  <- flush right edge
  {
    const auto L = computeCarousel(5, 2, screen, mt);
    near(static_cast<double>(L.visibleCards), 5, "A: five cards in the run");
    near(L.stripW, 1142.0, "A: stripW = 1580 * (1142/1580) = availableW");
    near(L.cards[2].x, 390.30379746835445, "A: selected x = 540*s");
    near(L.cards[1].x, 159.01265822784814, "A: near-left x = 220*s");
    near(L.cards[0].x, 0.0, "A: far-left x = 0*s (flush with the run's left)");
    near(L.cards[3].x, 708.3291139240506, "A: near-right x = 980*s");
    near(L.cards[4].x, 939.620253164557, "A: far-right x = 1300*s");

    near(L.cards[2].w, 361.3924050632911, "A: selected width = 500*s");
    near(L.cards[1].w, 274.65822784810126, "A: depth1 width = 380*s");
    near(L.cards[3].w, 274.65822784810126, "A: depth1 width (mirrored)");
    near(L.cards[0].w, 202.37974683544303, "A: depth2 width = 280*s");
    near(L.cards[4].w, 202.37974683544303, "A: depth2 width (mirrored)");

    // Flush at both ends, in the same nominal px as the x values above.
    near(L.cards[0].x, 0.0, "A: run starts at stripX 0");
    near(L.cards[4].x + L.cards[4].w, L.stripW, "A: run ends flush with stripW");

    // The ladder is monotone: every card out from the center is smaller.
    yes(L.cards[2].w > L.cards[1].w, "A: width shrinks, depth 0 -> 1");
    yes(L.cards[1].w > L.cards[0].w, "A: width shrinks, depth 1 -> 2");
  }

  // ---- B. ONE card height: the panel is fixed-aspect -----------------------
  // heightOf(slot) = width(|slot|)/aspect + (selected ? captionGap+captionH : 0),
  // so a card is `aspect` = 1.6 px wide per px tall (w/h = 1.6, h/w = 0.625):
  //   depth0 = 361.3924050632911/1.6 + 12 + 44 = 225.87025316455697 + 56
  //          = 281.87025316455697   <- frame + caption band, the tallest card
  //   depth1 = 274.65822784810126/1.6 = 171.66139240506328
  //   depth2 = 202.37974683544303/1.6 = 126.4873417721519
  // The selected card is widest AND is the only one with a band, so it is
  // always the tallest: stripH = max over the run = the selected card's h.
  // This is the whole point of the mode: the window is CONTAINED in the card at
  // draw time, so a wide-short window cannot move any card's box. The window's
  // own shape is not an input to the layout at all — hence the invariant is
  // expressible as a per-card ratio, not as a per-window one.
  {
    const auto L = computeCarousel(5, 2, screen, mt);
    const auto &sel = L.cards[2];
    near(sel.h, 281.87025316455697, "B: selected h = w/1.6 + 12 + 44");
    near(sel.h - 56.0, sel.w / 1.6, "B: selected frame is w/1.6 plus the band");
    near(L.stripH, sel.h, "B: stripH = selected card (widest, plus the band)");
    yes(sel.showCaption, "B: the selected card carries the caption");
    yes(L.cards[2].depth == 0, "B: the selected card is depth 0");

    for (const int i : {0, 1, 3, 4}) {
      const auto &c = L.cards[static_cast<size_t>(i)];
      yes(c.visible, "B: side card is in the run");
      near(c.w / c.h, 1.6, "B: side card is 1.6 wide per px tall", 1e-12);
      yes(!c.showCaption, "B: side card carries no caption band");
    }
  }

  // ---- C. the run always fits the screen height ----------------------------
  // Height budget: screen.h - 2*screenMargin - countH - sectionGap
  //              = 493 - 64 - 32 - 12 = 385 >= 281.87025316455697 -> hScale = 1,
  // so nothing shrinks on this screen.
  // Short screen 360: budget = 360 - 64 - 32 - 12 = 252 < 281.87025316455697,
  // so hScale = 252/281.87025316455697 = 0.894028359398682 and the panel lands
  // exactly on the limit: stripH*hScale = 252, panelH = 44 + 252 = 296 =
  // screen.h - 2*screenMargin. The width shrinks by the same factor:
  //   stripW*hScale = 1142*0.894028359398682 = 1020.9803864332948
  //   width(0)*hScale = 500*s*hScale = 323.0950589978781
  //   selected h*hScale = 252 (band included).
  // A uniform scale, so the depth ratios are untouched:
  //   width(0)/width(1) = 500/380, width(1)/width(2) = 380/280.
  {
    const auto S = computeCarousel(5, 2, LRect{0, 0, 1206, 360}, mt);
    near(S.stripH, 252.0, "C: short: stripH*hScale = height budget");
    near(S.panelH, 296.0, "C: short: panelH = countH+gap+stripH");
    yes(S.panelH <= 360.0 - 2.0 * mt.screenMargin + 1e-9,
        "C: short: run fits inside the vertical margins");
    near(S.stripW, 1020.9803864332948, "C: short: stripW shrinks with hScale");
    near(S.cards[2].w, 323.0950589978781, "C: short: selected card shrank");
    near(S.cards[2].w / S.cards[1].w, 500.0 / 380.0,
         "C: short: selected/near ratio kept");
    near(S.cards[1].w / S.cards[0].w, 380.0 / 280.0,
         "C: short: near/far ratio kept");
    // The one fixed aspect survives the shrink too.
    near(S.cards[1].w / S.cards[1].h, 1.6, "C: short: card is still 1.6", 1e-12);
    // Still flush at both ends after the shrink.
    near(S.cards[0].x, 0.0, "C: short: run still starts flush");
    near(S.cards[4].x + S.cards[4].w, S.stripW, "C: short: run still ends flush");

    // Tall screen: the height budget is not binding, so hScale is exactly 1 —
    // observable as the widths/heights being their unscaled wScale values.
    const auto T = computeCarousel(5, 2, LRect{0, 0, 1206, 1080}, mt);
    near(T.cards[2].w, 361.3924050632911, "C: tall: hScale is 1 (width)");
    near(T.cards[2].h, 281.87025316455697, "C: tall: hScale is 1 (height)");
    near(T.stripH, 281.87025316455697, "C: tall: stripH unscaled");
    yes(T.panelH <= 1080.0 - 2.0 * mt.screenMargin + 1e-9,
        "C: tall: run fits inside the vertical margins");
  }

  // ---- D. the selection wraps in both directions ---------------------------
  // sel = ((selected % count) + count) % count: -1 of 5 lands on 4, count of 5
  // lands on 0. Either way the selected card is the depth-0 one and the only
  // captioned one, and the run reads the same around it.
  {
    const auto N = computeCarousel(5, -1, screen, mt);
    near(static_cast<double>(N.cards[4].depth), 0,
         "D: selection -1 wraps to the last card");
    yes(N.cards[4].showCaption, "D: wrapped selection is captioned");
    near(static_cast<double>(N.cards[3].depth), 1, "D: card before it is depth1");
    near(static_cast<double>(N.cards[0].depth), 1,
         "D: card after the wrap is depth1");

    const auto O = computeCarousel(5, 5, screen, mt);
    near(static_cast<double>(O.cards[0].depth), 0,
         "D: selection == count wraps to 0");
    yes(O.cards[0].showCaption, "D: wrapped selection 0 is captioned");
    near(static_cast<double>(O.cards[4].depth), 1, "D: card before it is depth1");

    // No windows: nothing to lay out, and no crash.
    const auto Z = computeCarousel(0, 0, screen, mt);
    yes(Z.cards.empty(), "D: count 0 yields no cards");
    near(static_cast<double>(Z.visibleCards), 0,
         "D: count 0 yields no visible cards");
    near(Z.panelW, 0, "D: count 0 yields no panel");
    near(Z.stripH, 0, "D: count 0 yields no strip");
  }

  // ---- E. more windows than slots: only the run is laid out ----------------
  // count 9, visibleCards 5, selection 4 -> slots -2..2 map to indices
  // 2,3,4,5,6. The other four stay default-constructed: not visible, no box.
  {
    const auto L = computeCarousel(9, 4, screen, mt);
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
    near(static_cast<double>(L.cards[4].depth), 0,
         "E: the selected card is in the run");
  }

  // ---- F. side cards centre on the selected frame --------------------------
  // Every visible side card is offset by exactly half the height difference
  // against the SELECTED card's box — the selected card is the tall one
  // because it alone carries the caption band:
  //   depth1 y = (281.87025316455697 - 171.66139240506328)/2 = 55.104430379746844
  //   depth2 y = (281.87025316455697 - 126.4873417721519)/2  = 77.69145569620254
  // The selected card sits at 0: it is the reference box.
  // Checked as the rule itself (not just these two numbers), on the unscaled
  // run, on the height-shrunk run, and at every depth of a 9-slot run.
  {
    const auto L = computeCarousel(5, 2, screen, mt);
    near(L.cards[2].y, 0.0, "F: selected card is the reference box");
    near(L.cards[1].y, 55.104430379746844, "F: near-left y = (selH-h)/2");
    near(L.cards[3].y, 55.104430379746844, "F: near-right y = (selH-h)/2");
    near(L.cards[0].y, 77.69145569620254, "F: far-left y = (selH-h)/2");
    near(L.cards[4].y, 77.69145569620254, "F: far-right y = (selH-h)/2");

    const auto selH = L.cards[2].h;
    for (const auto &c : L.cards) {
      if (!c.visible || c.depth == 0)
        continue;
      near(c.y, (selH - c.h) / 2.0, "F: rule holds for every side card");
    }

    const auto S = computeCarousel(5, 2, LRect{0, 0, 1206, 360}, mt);
    const auto selHs = S.cards[2].h;
    for (const auto &c : S.cards) {
      if (!c.visible || c.depth == 0)
        continue;
      near(c.y, (selHs - c.h) / 2.0, "F: rule holds after the height shrink");
    }

    // Depth 3+ keeps shrinking by `taper` per step, and keeps the same two
    // invariants: width strictly decreasing outward, and 1.6 everywhere.
    // nominal = 500 + 2*((380-60)+(280-60)+(224-60)+(179.2-60))
    //         = 500 + 2*823.2 = 2146.4, so wScale = 1142/2146.4 here and the
    // depth widths are 500s, 380s, 224s, 179.2s with s = 1142/2146.4.
    CarouselMetrics m9 = mt;
    m9.visibleCards = 9;
    const auto D = computeCarousel(9, 4, screen, m9);
    near(static_cast<double>(D.visibleCards), 9, "F: nine slots in the run");
    for (int d = 1; d <= 4; ++d) {
      for (const int side : {-1, 1}) {
        const auto &c = D.cards[static_cast<size_t>(4 + side * d)];
        near(c.w / c.h, 1.6, "F: deep side card is 1.6 too", 1e-12);
        near(c.y, (D.cards[4].h - c.h) / 2.0, "F: deep side card centres too");
      }
      yes(D.cards[4].w > D.cards[static_cast<size_t>(4 - d)].w,
          "F: width shrinks strictly outward at every depth");
    }
  }

  if (g_failed) {
    std::printf("\n%d carousel layout check(s) FAILED\n", g_failed);
    return 1;
  }
  std::printf("all carousel layout checks passed\n");
  return 0;
}