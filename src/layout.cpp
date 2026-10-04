#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace gloview {

namespace {

LRect inset(const LRect& a, const LayoutCfg& c) {
    return LRect{a.x + c.padLeft, a.y + c.padTop, std::max(1.0, a.w - c.padLeft - c.padRight), std::max(1.0, a.h - c.padTop - c.padBottom)};
}

std::vector<int> rowCounts(int n, int rows) {
    std::vector<int> out(rows, n / rows);
    for (int i = 0; i < n % rows; ++i)
        ++out[i];
    return out;
}

struct RowPlan {
    double           scale = 0.0;
    std::vector<int> counts;
};

RowPlan planForRows(const std::vector<LRect>& w, const LRect& area, const LayoutCfg& cfg, int rows) {
    const auto counts = rowCounts(static_cast<int>(w.size()), rows);

    // Gaps are unscaled: the scale applies only to content left after carving
    // gaps out of the area, else wide rows overflow the area.
    double sW         = cfg.maxScale;
    double totalH     = 0.0;
    double totalGapsH = std::max(0, rows - 1) * cfg.gap;
    int    idx        = 0;
    for (int r = 0; r < rows; ++r) {
        double rowContentW = 0.0, rowH = 1.0;
        for (int c = 0; c < counts[r]; ++c, ++idx) {
            rowContentW += w[idx].w;
            rowH = std::max(rowH, w[idx].h);
        }
        const double rowGapsW = std::max(0, counts[r] - 1) * cfg.gap;
        const double availW   = std::max(1.0, area.w - rowGapsW);
        sW                    = std::min(sW, availW / std::max(1.0, rowContentW));
        totalH += rowH;
    }
    const double availH = std::max(1.0, area.h - totalGapsH);
    const double s      = std::max(0.01, std::min({sW, availH / std::max(1.0, totalH), cfg.maxScale}));
    return {s, counts};
}

std::vector<LRect> layoutRows(const std::vector<LRect>& w, const LRect& area, const LayoutCfg& cfg) {
    const int n = static_cast<int>(w.size());
    if (n == 0)
        return {};

    // Spatial reading order, unless cfg.preserveOrder: a caller-supplied order
    // (e.g. MRU for alt-tab) must survive untouched.
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    if (!cfg.preserveOrder) {
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (std::abs(w[a].cy() - w[b].cy()) > std::min(w[a].h, w[b].h) * 0.5)
                return w[a].cy() < w[b].cy();
            return w[a].cx() < w[b].cx();
        });
    }
    std::vector<LRect> sorted(n);
    for (int i = 0; i < n; ++i)
        sorted[i] = w[order[i]];

    RowPlan best;
    int     bestRows = 1;
    for (int rows = 1; rows <= n; ++rows) {
        const auto plan = planForRows(sorted, area, cfg, rows);
        if (plan.scale > best.scale) {
            best     = plan;
            bestRows = rows;
        }
    }

    const double s = best.scale;
    double totalH = 0.0;
    {
        int idx = 0;
        for (int r = 0; r < bestRows; ++r) {
            double rowH = 1.0;
            for (int c = 0; c < best.counts[r]; ++c, ++idx)
                rowH = std::max(rowH, sorted[idx].h);
            totalH += rowH * s + (r > 0 ? cfg.gap : 0.0);
        }
    }

    std::vector<LRect> outSorted(n);
    double             y   = area.y + (area.h - totalH) / 2.0;
    int                idx = 0;
    for (int r = 0; r < bestRows; ++r) {
        double rowW = 0.0, rowH = 1.0;
        for (int c = 0; c < best.counts[r]; ++c) {
            rowW += sorted[idx + c].w * s + (c > 0 ? cfg.gap : 0.0);
            rowH = std::max(rowH, sorted[idx + c].h);
        }
        rowH *= s;
        double x = area.x + (area.w - rowW) / 2.0;
        for (int c = 0; c < best.counts[r]; ++c, ++idx) {
            const double tw = sorted[idx].w * s;
            const double th = sorted[idx].h * s;
            outSorted[idx]  = LRect{x, y + (rowH - th) / 2.0, tw, th};
            x += tw + cfg.gap;
        }
        y += rowH + cfg.gap;
    }

    std::vector<LRect> out(n);
    for (int i = 0; i < n; ++i)
        out[order[i]] = outSorted[i];
    return out;
}

std::vector<LRect> layoutGrid(const std::vector<LRect>& w, const LRect& area, const LayoutCfg& cfg) {
    const int n = static_cast<int>(w.size());
    if (n == 0)
        return {};
    const int    cols  = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
    const int    rows  = std::max(1, static_cast<int>(std::ceil(static_cast<double>(n) / cols)));
    const double cellW = (area.w - (cols - 1) * cfg.gap) / cols;
    const double cellH = (area.h - (rows - 1) * cfg.gap) / rows;

    std::vector<LRect> out(n);
    for (int i = 0; i < n; ++i) {
        const int    r     = i / cols;
        const int    c     = i % cols;
        const double slotX = area.x + c * (cellW + cfg.gap);
        const double slotY = area.y + r * (cellH + cfg.gap);
        const double scale = std::min({cellW / w[i].w, cellH / w[i].h, cfg.maxScale});
        const double tw    = w[i].w * scale;
        const double th    = w[i].h * scale;
        out[i]             = LRect{slotX + (cellW - tw) / 2.0, slotY + (cellH - th) / 2.0, tw, th};
    }
    return out;
}

std::vector<LRect> layoutNatural(const std::vector<LRect>& w, const LRect& area, const LayoutCfg& cfg) {
    const int n = static_cast<int>(w.size());
    if (n == 0)
        return {};
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (const auto& r : w) {
        minX = std::min(minX, r.x);
        minY = std::min(minY, r.y);
        maxX = std::max(maxX, r.x + r.w);
        maxY = std::max(maxY, r.y + r.h);
    }
    const double bw = std::max(1.0, maxX - minX);
    const double bh = std::max(1.0, maxY - minY);
    const double s  = std::min({area.w / bw, area.h / bh, cfg.maxScale});
    const double ox = area.x + (area.w - bw * s) / 2.0;
    const double oy = area.y + (area.h - bh * s) / 2.0;

    std::vector<LRect> out(n);
    for (int i = 0; i < n; ++i)
        out[i] = LRect{ox + (w[i].x - minX) * s, oy + (w[i].y - minY) * s, w[i].w * s, w[i].h * s};
    return out;
}

} // namespace

std::vector<LRect> computeLayout(const std::vector<LRect>& naturals, const LRect& area, const LayoutCfg& cfg) {
    const LRect a = inset(area, cfg);
    switch (cfg.engine) {
        case Engine::Grid: return layoutGrid(naturals, a, cfg);
        case Engine::Natural: return layoutNatural(naturals, a, cfg);
        case Engine::Rows:
        default: return layoutRows(naturals, a, cfg);
    }
}

Engine parseEngine(const char* s) {
    if (!s)
        return Engine::Rows;
    if (std::strcmp(s, "grid") == 0)
        return Engine::Grid;
    if (std::strcmp(s, "natural") == 0)
        return Engine::Natural;
    if (std::strcmp(s, "carousel") == 0)
        return Engine::Carousel;
    return Engine::Rows;
}

// Depths 0-2 are fixed widths; past that they taper geometrically, so every
// card out is strictly smaller than the one inside it.
static double cardWidth(int depth, const CarouselMetrics& mt) {
    if (depth <= 0)
        return mt.selectedW;
    if (depth == 1)
        return mt.nearW;
    return mt.farW * std::pow(mt.taper, static_cast<double>(depth - 2));
}

CarouselLayout computeCarousel(int count, int selected, const LRect& screen,
                               const std::vector<double>& aspect,
                               const CarouselMetrics& mt) {
    CarouselLayout L;
    L.area = screen;
    if (count <= 0)
        return L;
    L.cards.resize(static_cast<size_t>(count));
    L.countW = mt.countW;
    L.countH = mt.countH;

    const auto aspectOf = [&](int i) {
        return (i < static_cast<int>(aspect.size()) && aspect[static_cast<size_t>(i)] > 0.0)
                   ? aspect[static_cast<size_t>(i)] : 16.0 / 9.0;
    };

    // Even counts are allowed: centerSlot leaves the selected card one slot off the
    // exact middle.
    const int want = std::max(1, mt.visibleCards);
    const int shown = std::min(want, count);
    L.visibleCards = shown;

    const double availableW = std::max(1.0, screen.w - mt.screenMargin * 2.0);
    const int centerSlot = shown / 2;
    const int leftSlots = centerSlot;
    const int rightSlots = shown - 1 - centerSlot;

    double nominal = mt.selectedW;
    for (int d = 1; d <= leftSlots; ++d)
        nominal += cardWidth(d, mt) - mt.overlap;
    for (int d = 1; d <= rightSlots; ++d)
        nominal += cardWidth(d, mt) - mt.overlap;

    const double wScale = std::min(1.0, availableW / std::max(1.0, nominal));
    auto width = [&](int depth) { return cardWidth(depth, mt) * wScale; };
    const double overlap = mt.overlap * wScale;

    // Slot -> tile index, so the vertical pass below sees the same assignment.
    const int sel = ((selected % count) + count) % count;
    std::vector<int> slotOf(static_cast<size_t>(count), 99);
    std::vector<int> idxOfSlot;
    idxOfSlot.reserve(static_cast<size_t>(shown));
    for (int rel = -leftSlots; rel <= rightSlots; ++rel) {
        const int idx = ((sel + rel) % count + count) % count;
        slotOf[static_cast<size_t>(idx)] = rel;
        idxOfSlot.push_back(idx);
    }

    L.stripW = width(0);
    for (int d = 1; d <= leftSlots; ++d)
        L.stripW += width(d) - overlap;
    for (int d = 1; d <= rightSlots; ++d)
        L.stripW += width(d) - overlap;

    // Card height is the WINDOW's, not a fixed ratio: a tile box that does not
    // carry the window's aspect cannot be drawn by the shared tile draw.
    const auto heightOf = [&](int idx, int slot) {
        const double w = width(std::abs(slot));
        const double h = w / aspectOf(idx);
        return h + (slot == 0 ? mt.captionGap + mt.captionH : 0.0);
    };
    double frameH = 0.0;
    for (const int idx : idxOfSlot)
        frameH = std::max(frameH, heightOf(idx, slotOf[static_cast<size_t>(idx)]));
    L.stripH = frameH;

    const double maxPanelH = std::max(1.0, screen.h - mt.screenMargin * 2.0);
    const double hScale =
        std::min(1.0, std::max(1.0, maxPanelH - mt.countH - mt.sectionGap) /
                           std::max(1.0, L.stripH));
    L.stripW *= hScale;
    L.stripH *= hScale;

    L.panelW = L.stripW;
    L.panelH = mt.countH + mt.sectionGap + L.stripH;
    L.countX = (L.panelW - L.countW) * 0.5;
    L.stripY = mt.countH + mt.sectionGap;
    L.originX = std::round((screen.w - L.panelW) * 0.5);
    L.originY = std::round((screen.h - L.panelH) * 0.5);

    const double selX = (L.stripW - width(0) * hScale) * 0.5;
    const double selFrameH = heightOf(sel, 0) * hScale;

    for (const int idx : idxOfSlot) {
        const int slot = slotOf[static_cast<size_t>(idx)];
        const int dist = std::abs(slot);
        CarouselLayout::Card& c = L.cards[static_cast<size_t>(idx)];
        c.visible = true;
        c.showCaption = slot == 0;
        c.depth = dist;
        const double w = width(dist) * hScale;
        double x = selX;
        const int step = slot < 0 ? -1 : 1;
        for (int k = 1; k <= dist; ++k) {
            const double prevW = width(k - 1) * hScale;
            const double curW = width(k) * hScale;
            x = step < 0 ? x - curW + overlap * hScale
                         : x + prevW - overlap * hScale;
        }
        c.x = x;
        c.w = w;
        c.h = heightOf(idx, slot) * hScale;
        // Side cards centre their box against the SELECTED card's box: the
        // selected card is taller (it carries the caption).
        c.y = (selFrameH - c.h) * 0.5;
    }
    return L;
}

} // namespace gloview
