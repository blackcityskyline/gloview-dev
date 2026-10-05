#pragma once

#include <algorithm>
#include <vector>

// pure geometry, monitor-local LOGICAL px
namespace gloview {

struct LRect {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;

    [[nodiscard]] bool contains(double px, double py) const {
        return px >= x && py >= y && px <= x + w && py <= y + h;
    }
    void grow(double d) {
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
    Rows,
    Grid,
    Natural,
};

struct LayoutCfg {
    Engine engine    = Engine::Rows;
    double padTop    = 60.0;
    double padRight  = 80.0;
    double padBottom = 70.0;
    double padLeft   = 80.0;
    double gap       = 36.0;
    double maxScale  = 1.0;
    bool   preserveOrder = false;
};

// result parallel to `naturals`, inside `area`
std::vector<LRect> computeLayout(const std::vector<LRect>& naturals, const LRect& area, const LayoutCfg& cfg);

Engine parseEngine(const char* s);

// ONE standard box for every card: the size a fullscreen window occupies, to the
// pixel. Every window is drawn inside that same box — a smaller one fitted whole,
// neither deformed — so the run's geometry never depends on which window is
// centred, and no window ever spills past a ring (which no corner radius could
// hide). The centred card is the only one framed.
struct CarouselMetrics {
    double boxW          = 0.0; // standard card width; 0 = fill the usable area
    double boxH          = 0.0; // standard card height; 0 = follow boxW
    double pitchK        = 0.816; // step to a neighbour, as a fraction of its width (<1 overlaps)
    double nearK         = 0.76;  // depth 1 size, as a fraction of the standard box
    double farK          = 0.56;  // depth 2
    double taper         = 0.8;   // every depth past 2
    double scale         = 1.0;   // user zoom on the whole run, over the fit
    double captionH      = 44.0;
    double captionGap    = 12.0;
    double countH        = 32.0;
    double countW        = 88.0;
    double sectionGap    = 12.0;
    double screenMargin  = 32.0;
    int    visibleCards  = 5;
};

struct CarouselLayout {
    struct Card {
        bool   visible = false;
        bool   showCaption = false;
        int    depth = 3;      // 0 = selected, 1 = near, 2+ farther
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    };
    std::vector<Card> cards;
    int    visibleCards = 0;
    double panelW = 0.0, panelH = 0.0;
    double stripY = 0.0, stripW = 0.0, stripH = 0.0;
    double countX = 0.0, countY = 0.0, countW = 0.0, countH = 0.0;
    double originX = 0.0, originY = 0.0;
    LRect area;
};

// `selected` = centered card index, wrapped
CarouselLayout computeCarousel(int count, int selected, const LRect& screen,
                               const CarouselMetrics& mt = {});

}
