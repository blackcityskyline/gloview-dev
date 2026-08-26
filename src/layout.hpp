#pragma once

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

} // namespace gloview
