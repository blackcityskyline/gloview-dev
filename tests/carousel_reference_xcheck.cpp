// Differential check: our layout.cpp port vs noctalia v5.2's formula,
// transcribed here INDEPENDENTLY and verbatim from
// src/shell/switcher/window_switcher_carousel_style.cpp + ui/style.h.
// Sweeps screen sizes x window counts x selections; every resolved number
// (card x/y/w/h, depth, panel, strip, counter, origin) must match.
//
// Not part of the plugin build — it links layout.cpp alone, which is only
// possible because layout.hpp/cpp stay Hyprland-free. See
// tests/layout_carousel_check.cpp for the human-readable assertions on the
// same numbers; this file exists to prove the PORT, not the intent.

#include "../src/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace gloview;

namespace {

// Style constants, src/ui/style.h (noctalia v5.2).
constexpr float kSpaceXs = 4.0F;
constexpr float kSpaceSm = 8.0F;
constexpr float kSpaceMd = 12.0F;
constexpr float kSpaceLg = 16.0F;
constexpr float kControlHeightSm = 32.0F;
constexpr float kControlHeightLg = 44.0F;
constexpr float kSelectedCardWidth = 500.0F;
constexpr float kNearCardWidth = 380.0F;
constexpr float kFarCardWidth = 280.0F;
constexpr float kPreviewAspect = 1.6F;
constexpr float kCardOverlap = kControlHeightLg + kSpaceLg;
constexpr float kNarrowThreshold = 0.72F;
constexpr float kCaptionHeight = kControlHeightLg;
constexpr int kVisibleCards = 5;
constexpr int kHeightFitPasses = 2;
constexpr float kMinExtent = 1.0F;

struct Card {
  float x = 0, y = 0, w = 0, h = 0;
  int depth = 0;
  bool visible = false;
};

struct Layout {
  Card cards[32]{};
  int visibleCards = 0;
  float panelW = 0, panelH = 0, stripW = 0, stripH = 0;
  float countX = 0, countY = 0, countW = 0, countH = 0;
  float stripX = 0, stripY = 0;
  float ox = 0, oy = 0;
};

// computeWindowSwitcherCarouselLayout, transcribed.
float previewFrameHeight(float cardWidth, float scale) {
  const float frameInset = kSpaceXs * scale;
  const float previewWidth = std::max(0.0F, cardWidth - frameInset * 2.0F);
  return previewWidth / kPreviewAspect + frameInset * 2.0F;
}

Layout reference(float screenW, float screenH, int count, int selected,
                 float scale) {
  Layout L;
  if (count <= 0)
    return L;

  const float screenMargin = kControlHeightSm * scale;
  const float countHeight = kControlHeightSm * scale;
  const float countWidth = kControlHeightLg * scale * 2.0F;
  const float sectionGap = kSpaceMd * scale;
  const float captionHeight = kCaptionHeight * scale;
  const float captionGap = kSpaceMd * scale;
  L.visibleCards =
      static_cast<int>(std::max<std::size_t>(
          1, std::min<std::size_t>(kVisibleCards, static_cast<std::size_t>(count))));

  const float availableWidth =
      std::max(kMinExtent, screenW - screenMargin * 2.0F);
  auto desiredWidth = [&] {
    const std::size_t sideSlots = L.visibleCards / 2;
    float width = kSelectedCardWidth;
    if (sideSlots >= 1)
      width += kNearCardWidth * 2.0F;
    if (sideSlots >= 2)
      width += kFarCardWidth * 2.0F;
    width -= kCardOverlap * static_cast<float>(sideSlots * 2);
    return width * scale;
  };
  while (L.visibleCards > 3 &&
         availableWidth / desiredWidth() < kNarrowThreshold)
    --L.visibleCards;

  const float widthScale = std::min(1.0F, availableWidth / desiredWidth());
  float selectedWidth = kSelectedCardWidth * scale * widthScale;
  float nearWidth = kNearCardWidth * scale * widthScale;
  float farWidth = kFarCardWidth * scale * widthScale;
  float overlap = kCardOverlap * scale * widthScale;

  auto updateExtents = [&] {
    const std::size_t sideSlots = L.visibleCards / 2;
    L.stripW = selectedWidth;
    if (sideSlots >= 1)
      L.stripW += (nearWidth - overlap) * 2.0F;
    if (sideSlots >= 2)
      L.stripW += (farWidth - overlap) * 2.0F;
    L.stripH = previewFrameHeight(selectedWidth, scale) + captionGap +
               captionHeight;
    L.panelW = L.stripW;
    L.panelH = countHeight + sectionGap + L.stripH;
  };
  updateExtents();

  const float maxPanelHeight =
      std::max(kMinExtent, screenH - screenMargin * 2.0F);
  for (int pass = 0; pass < kHeightFitPasses && L.panelH > maxPanelHeight;
       ++pass) {
    const float availableStripHeight = std::max(
        kMinExtent, maxPanelHeight - countHeight - sectionGap);
    const float heightScale =
        std::min(1.0F, availableStripHeight / L.stripH);
    selectedWidth *= heightScale;
    nearWidth *= heightScale;
    farWidth *= heightScale;
    overlap *= heightScale;
    updateExtents();
  }

  L.countW = countWidth;
  L.countH = countHeight;
  L.countX = (L.panelW - countWidth) * 0.5F;
  L.countY = 0.0F;
  L.stripX = 0.0F;
  L.stripY = countHeight + sectionGap;
  L.ox = std::round((screenW - L.panelW) * 0.5F);
  L.oy = std::round((screenH - L.panelH) * 0.5F);

  const std::size_t visibleCount =
      std::min<std::size_t>(L.visibleCards, static_cast<std::size_t>(count));
  const std::size_t centerSlot = visibleCount / 2;
  const std::size_t start = (selected % count + count -
                             static_cast<int>(centerSlot % count) + count) %
                            count;
  const float selectedX = (L.stripW - selectedWidth) * 0.5F;
  for (std::size_t slot = 0; slot < visibleCount; ++slot) {
    const std::size_t idx = (start + slot) % count;
    const long rel = static_cast<long>(slot) - static_cast<long>(centerSlot);
    const auto distance =
        static_cast<std::size_t>(std::abs(static_cast<int>(rel)));
    Card &c = L.cards[idx];
    c.visible = true;
    c.depth = static_cast<int>(distance);
    if (distance == 0) {
      c.w = selectedWidth;
      c.x = selectedX;
    } else if (distance == 1) {
      c.w = nearWidth;
      c.x = rel < 0 ? selectedX - nearWidth + overlap
                    : selectedX + selectedWidth - overlap;
    } else {
      c.w = farWidth;
      const float nearLeft = selectedX - nearWidth + overlap;
      const float nearRight = selectedX + selectedWidth - overlap;
      c.x = rel < 0 ? nearLeft - farWidth + overlap
                    : nearRight + nearWidth - overlap;
    }
    const float previewHeight = previewFrameHeight(c.w, scale);
    c.h = previewHeight;
    if (distance == 0)
      c.h += captionGap + captionHeight;
    const float selectedPreviewHeight =
        previewFrameHeight(selectedWidth, scale);
    c.y = (selectedPreviewHeight - previewHeight) * 0.5F;
  }
  return L;
}

int g_failed = 0;
double g_worst = 0.0;

// The reference computes in `float`, we compute in `double` — the same
// expression tree then differs by float rounding, which at these magnitudes
// (card coords ~1e3, accumulated through two width-fit passes) is ~1e-4.
// Anything above a few float ULPs is a real divergence, not that noise.
constexpr double kTol = 5e-4;

void check(const char *what, double got, double want, double sw, double sh,
           int n, int sel) {
  const double d = std::abs(got - want);
  if (d > g_worst)
    g_worst = d;
  if (d > kTol) {
    std::printf("MISMATCH %-10s sw=%.0f sh=%.0f n=%2d sel=%3d  got %.5f  want %.5f\n",
                what, sw, sh, n, sel, got, want);
    ++g_failed;
  }
}

} // namespace

int main() {
  // Our metrics are authored at shell UI scale 1 (logical px); the reference
  // multiplies everything by its own `scale`, so compare at 1.
  const CarouselMetrics mt;
  int cases = 0;
  for (const float sw : {640.F, 800.F, 1000.F, 1280.F, 1366.F, 1600.F, 1920.F,
                         2560.F, 3440.F}) {
    for (const float sh : {480.F, 600.F, 720.F, 900.F, 1080.F, 1440.F, 2160.F}) {
      for (const int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 20}) {
        for (const int sel : {0, 1, 3, 7, -1, -5}) {
          const auto R = reference(sw, sh, n, sel, 1.0F);
          const auto M = computeCarousel(n, sel, LRect{0, 0, sw, sh}, mt);
          ++cases;
          check("vis", M.visibleCards, R.visibleCards, sw, sh, n, sel);
          check("panelW", M.panelW, R.panelW, sw, sh, n, sel);
          check("panelH", M.panelH, R.panelH, sw, sh, n, sel);
          check("stripW", M.stripW, R.stripW, sw, sh, n, sel);
          check("stripH", M.stripH, R.stripH, sw, sh, n, sel);
          check("stripX", M.stripX, R.stripX, sw, sh, n, sel);
          check("stripY", M.stripY, R.stripY, sw, sh, n, sel);
          check("countX", M.countX, R.countX, sw, sh, n, sel);
          check("countY", M.countY, R.countY, sw, sh, n, sel);
          check("countW", M.countW, R.countW, sw, sh, n, sel);
          check("countH", M.countH, R.countH, sw, sh, n, sel);
          check("ox", M.originX, R.ox, sw, sh, n, sel);
          check("oy", M.originY, R.oy, sw, sh, n, sel);
          for (int i = 0; i < n && i < 32; ++i) {
            check("vis?", M.cards[i].visible, R.cards[i].visible, sw, sh, n,
                  sel);
            // Only IN-RUN cards carry geometry or a depth: the reference
            // leaves those fields untouched for a card outside the visible
            // window (no assignment in the slot loop), so comparing them
            // would just measure our default initializers.
            if (!M.cards[i].visible)
              continue;
            check("depth", M.cards[i].depth, R.cards[i].depth, sw, sh, n, sel);
            check("x", M.cards[i].x, R.cards[i].x, sw, sh, n, sel);
            check("y", M.cards[i].y, R.cards[i].y, sw, sh, n, sel);
            check("w", M.cards[i].w, R.cards[i].w, sw, sh, n, sel);
            check("h", M.cards[i].h, R.cards[i].h, sw, sh, n, sel);
          }
        }
      }
    }
  }
  std::printf("%s  %d layouts, %d values each, max abs delta %.2e\n",
              g_failed ? "MISMATCHES FOUND" : "EXACT MATCH vs reference", cases,
              13 + 6 * 20, g_worst);
  return g_failed ? 1 : 0;
}
