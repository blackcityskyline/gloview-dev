#pragma once

#include <algorithm>
#include <vector>

// Pure, Hyprland-independent layout math for the overview. Everything here is
// plain geometry in monitor-local logical pixels so it can be unit-reasoned and
// tweaked without touching the renderer. Add a new engine by extending
// `Engine` and `computeLayout`.

namespace gloview {

struct LRect {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;

    [[nodiscard]] bool contains(double px, double py) const {
        return px >= x && py >= y && px <= x + w && py <= y + h;
    }
    void grow(double d) { // expand by d on every side (can go negative)
      x -= d;
      y -= d;
      w += 2 * d;
      h += 2 * d;
    }
    [[nodiscard]] double cx() const { return x + w / 2.0; }
    [[nodiscard]] double cy() const { return y + h / 2.0; }
    [[nodiscard]] double aspect() const { return h > 0.0 ? w / h : 1.0; }
};

inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline LRect lerp(const LRect &a, const LRect &b, double t) {
    return LRect{lerp(a.x, b.x, t), lerp(a.y, b.y, t),
                 lerp(a.w, b.w, t), lerp(a.h, b.h, t)};
}

enum class Engine {
    Rows,    // macOS-like: aspect-preserving, packed into balanced rows
    Grid,    // uniform cells, aspect-preserving inside each
    Natural, // keep relative on-screen position, uniformly scaled to fit
    Carousel, // fixed-width cards in an overlapped horizontal run, the
              // selected one centered (noctalia's window switcher)
};

struct LayoutCfg {
    Engine engine    = Engine::Rows;
    double padTop    = 60.0; // space already reserved above (e.g. strip) is added by caller
    double padRight  = 80.0;
    double padBottom = 70.0;
    double padLeft   = 80.0;
    double gap       = 36.0;  // min spacing between tiles
    double maxScale  = 1.0;   // never blow a window up past this * its real size
    // Rows engine normally re-sorts tiles into spatial reading order (top-to-bottom,
    // left-to-right by their NATURAL on-screen position), ignoring input order. When true,
    // the caller's input order is authoritative instead (row-major fill, no re-sort) — used
    // by the Alt-Tab grid so it visually reflects MRU order rather than screen position.
    bool   preserveOrder = false;
};

// `naturals` are the windows' real monitor-local rects; result is parallel,
// each entry the tile's target rect inside `area`. `area` is the full usable
// region (caller passes the monitor box; paddings are applied internally).
std::vector<LRect> computeLayout(const std::vector<LRect>& naturals, const LRect& area, const LayoutCfg& cfg);

Engine parseEngine(const char* s);

// ---- carousel ---------------------------------------------------------------
// Fixed-width cards in one horizontal overlapped run: the selected card sits in
// the center slot, its neighbours peek out on both sides, everything past the
// visible count is dropped. Geometry is ported 1:1 from noctalia v5.2's window
// switcher (src/shell/switcher/window_switcher_carousel_style.cpp +
// ui/style.h) so cards land exactly where they land there, in logical px at
// shell UI scale 1 (their `context.scale` multiplies card widths and the frame
// inset alike, leaving every ratio — and so every derived box — unchanged).
//
// The ONE deliberate divergence from the reference: the preview box has a
// FIXED aspect and the window is CONTAINED (letterboxed) inside it, so a 16:10
// window and a 9:21 window both sit whole inside the same card. Noctalia feeds
// the same fixed-aspect box a screencopy thumbnail scaled with ImageFit::Contain
// — identical landing.
struct CarouselMetrics {
    double selectedW     = 500.0; // windowSwitcherSelectedCardWidth
    double nearW         = 380.0; // windowSwitcherNearCardWidth
    double farW          = 280.0; // windowSwitcherFarCardWidth
    // Past depth 2 the reference has no width of its own (it reuses farW), so a
    // run deeper than 5 cards repeats the same size — read as two identical
    // cards at the end. Continue the shrink geometrically instead, anchored at
    // farW so depths 0..2 stay bit-identical to the reference.
    double taper         = 0.8;
    double overlap       = 60.0;  // windowSwitcherCardOverlap (ctrlH + spaceLg)
    double aspect        = 1.6;   // windowSwitcherPreviewAspect (w/h)
    double frameInset    = 4.0;   // spaceXs — card padding around the preview
    double captionH      = 44.0;  // windowSwitcherCaptionHeight (ctrlH)
    double captionGap    = 12.0;  // spaceMd
    double countH        = 32.0;  // controlHeightSm
    double countW        = 88.0;  // controlHeightLg * 2
    double sectionGap    = 12.0;  // spaceMd
    double screenMargin  = 32.0;  // controlHeightSm
    int    visibleCards  = 5;     // kVisibleCards — the reference's value; the
                                  // configured count, and any run up to it, is
                                  // laid out by the same ladder
};

// Resolved per layout pass. `cards` is parallel to the window list; the rest is
// panel-local — add originX/originY to place it on the monitor.
struct CarouselLayout {
    struct Card {
        bool   visible = false; // inside the visible run? (else: draw nothing)
        bool   showCaption = false; // only the centered card carries one
        int    depth = 3;      // 0 = selected, 1 = near, 2+ = progressively far
        int    direction = 0;  // -1 left of center, +1 right, 0 selected
        int    zIndex = 0;     // 3 - min(depth, 3)
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    };
    std::vector<Card> cards;
    int    visibleCards = 0;
    double panelW = 0.0, panelH = 0.0;
    double stripX = 0.0, stripY = 0.0, stripW = 0.0, stripH = 0.0;
    double countX = 0.0, countY = 0.0, countW = 0.0, countH = 0.0;
  double originX = 0.0, originY = 0.0; // panel top-left, centered in `area`
  LRect area;                        // the `screen` rect it was computed for
};

// `selected` is the centered card's index (wrapped). `screen` is the monitor
// box in logical px. Cards come out panel-local; callers offset by origin.
// `count` windows, `selected` centered (wrapped), in the `screen` box. Cards
// come out panel-local; callers offset by origin. `mt.visibleCards` is how many
// cards the run SHOWS at most (any count, even included); windows beyond it are
// simply not drawn. `mt.selectedW` is the centered card's width in logical px,
// and the whole run scales down from there to fit `screen`.
CarouselLayout computeCarousel(int count, int selected, const LRect& screen,
                               const CarouselMetrics& mt = {});

// The fixed-aspect preview box inside a card — the exact landing box for the
// window's content, which is then contained inside it. Shared by layoutTiles
// (targets) and the render path so the two can never disagree.
inline LRect carouselPreviewBox(const LRect& card, const CarouselMetrics& mt) {
    const double innerW = std::max(0.0, card.w - mt.frameInset * 2.0);
    return LRect{card.x + mt.frameInset, card.y + mt.frameInset, innerW,
                 innerW / mt.aspect};
}

} // namespace gloview
