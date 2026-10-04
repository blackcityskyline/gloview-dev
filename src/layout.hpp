#pragma once

#include <algorithm>
#include <vector>

// Pure layout math, Hyprland-independent: plain geometry in monitor-local
// logical pixels, so it can be reasoned about without the renderer. New engine
// = extend `Engine` and `computeLayout`.

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
    Carousel, // fixed-width cards in an overlapped run, the selected one centered
};

struct LayoutCfg {
    Engine engine    = Engine::Rows;
    double padTop    = 60.0;
    double padRight  = 80.0;
    double padBottom = 70.0;
    double padLeft   = 80.0;
    double gap       = 36.0;  // min spacing between tiles
    double maxScale  = 1.0;   // never blow a window up past this * its real size
    // Rows normally re-sorts into spatial reading order, ignoring input order;
    // true makes the caller's order authoritative (Alt-Tab's MRU order).
    bool   preserveOrder = false;
};

// `naturals` are the windows' real rects; the result is parallel, each entry a
// target rect inside `area` (the full monitor box — paddings apply internally).
std::vector<LRect> computeLayout(const std::vector<LRect>& naturals, const LRect& area, const LayoutCfg& cfg);

Engine parseEngine(const char* s);

// ---- carousel ---------------------------------------------------------------
// Fixed-width cards in one overlapped horizontal run, the selected card in the
// center. The preview box has a FIXED aspect and the window is CONTAINED
// (letterboxed) inside it, so a 16:10 and a 9:21 window both sit whole inside
// the same card. `selectedW` sets the centered card; the ladder keeps fixed
// ratios to it. `visibleCards` is how many cards the run shows — any count,
// even included — and the run scales down uniformly to fit.
struct CarouselMetrics {
    double selectedW     = 500.0;
    double nearW         = 380.0;
    double farW          = 280.0;
    // Past depth 2 the shrink continues geometrically, so every card out is
    // strictly smaller than the one inside it.
    double taper         = 0.8;
    double overlap       = 60.0;  // how far each card tucks under its neighbor
    double aspect        = 1.6;   // preview box w/h
    double frameInset    = 4.0;   // card padding around the preview
    double captionH      = 44.0;
    double captionGap    = 12.0;
    double countH        = 32.0;
    double countW        = 88.0;
    double sectionGap    = 12.0;
    double screenMargin  = 32.0;
    int    visibleCards  = 5;
};

// Resolved per layout pass. `cards` is parallel to the window list; the rest is
// panel-local — add originX/originY to place it on the monitor.
struct CarouselLayout {
    struct Card {
        bool   visible = false; // inside the run? (else: draw nothing)
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
    LRect area;                        // the rect it was computed for
};

// `selected` is the centered card's index (wrapped). Windows past
// `mt.visibleCards` come back invisible and are simply not drawn.
CarouselLayout computeCarousel(int count, int selected, const LRect& screen,
                               const CarouselMetrics& mt = {});

// The fixed-aspect preview box inside a card — the window's landing box.
// Shared by layoutTiles and the render path so they cannot disagree.
inline LRect carouselPreviewBox(const LRect& card, const CarouselMetrics& mt) {
    const double innerW = std::max(0.0, card.w - mt.frameInset * 2.0);
    return LRect{card.x + mt.frameInset, card.y + mt.frameInset, innerW,
                 innerW / mt.aspect};
}

} // namespace gloview
