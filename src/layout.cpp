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

// Split `n` items into `rows` near-equal groups, earlier rows get the extra.
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

// For a fixed row count, the best uniform scale that fits every row in width
// and the stack in height, given per-window natural sizes (in input order).
RowPlan planForRows(const std::vector<LRect>& w, const LRect& area, const LayoutCfg& cfg, int rows) {
    const auto counts = rowCounts(static_cast<int>(w.size()), rows);

    // Gaps are a fixed pixel size and are NOT scaled with the tiles, so the
    // scale must apply only to the tile content that remains after the gaps are
    // carved out of the area — otherwise wide rows overflow and tiles bleed off
    // screen / overlap.
    double sW         = cfg.maxScale;
    double totalH     = 0.0; // unscaled stacked content height (max per row)
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

    // Sorted reading order (top-to-bottom, then left-to-right) keeps tiles near where the
    // real windows live, which reads more naturally than raw order — UNLESS the caller
    // already put them in a meaningful order (e.g. MRU for Alt-Tab) and wants it preserved;
    // cfg.preserveOrder skips this spatial re-sort so row-major fill reflects input order.
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

    // pick the row count that yields the largest tiles.
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
    // total stack height at this scale, to vertically center.
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
            outSorted[idx]  = LRect{x, y + (rowH - th) / 2.0, tw, th}; // baseline-center within row
            x += tw + cfg.gap;
        }
        y += rowH + cfg.gap;
    }

    // unsort back to input order
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
    // bounding box of all naturals
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

// Ported 1:1 from noctalia v5.2's computeWindowSwitcherCarouselLayout
// (src/shell/switcher/window_switcher_carousel_style.cpp) — see the
// CarouselMetrics comment in layout.hpp for the constant-by-constant mapping.
CarouselLayout computeCarousel(int count, int selected, const LRect& screen,
                               const CarouselMetrics& mt) {
    CarouselLayout L;
    if (count <= 0)
        return L;
    L.cards.resize(static_cast<size_t>(count));
    L.countW = mt.countW;
    L.countH = mt.countH;
    L.visibleCards = std::max(1, std::min(mt.visibleCards, count));

    const double availableW = std::max(1.0, screen.w - mt.screenMargin * 2.0);
    // Width the full visible run needs at the nominal card widths.
    auto desiredWidth = [&] {
        const int sideSlots = L.visibleCards / 2;
        double width = mt.selectedW;
        if (sideSlots >= 1) width += mt.nearW * 2.0;
        if (sideSlots >= 2) width += mt.farW * 2.0;
        width -= mt.overlap * static_cast<double>(sideSlots * 2);
        return width;
    };
    // Too tight for 5 (or 4) cards: shed the outermost slot, never below 3.
    while (L.visibleCards > 3 && availableW / desiredWidth() < mt.narrowRatio)
        --L.visibleCards;

    const double widthScale = std::min(1.0, availableW / desiredWidth());
    double selW = mt.selectedW * widthScale;
    double nearW = mt.nearW * widthScale;
    double farW = mt.farW * widthScale;
    double overlap = mt.overlap * widthScale;

    // Card height from its width: the preview box is a fixed-aspect rect inset
    // by frameInset on every side.
    auto frameH = [&](double cardW) {
        const double innerW = std::max(0.0, cardW - mt.frameInset * 2.0);
        return innerW / mt.aspect + mt.frameInset * 2.0;
    };

    auto updateExtents = [&] {
        const int sideSlots = L.visibleCards / 2;
        L.stripW = selW;
        if (sideSlots >= 1) L.stripW += (nearW - overlap) * 2.0;
        if (sideSlots >= 2) L.stripW += (farW - overlap) * 2.0;
        L.stripH = frameH(selW) + mt.captionGap + mt.captionH;
        L.panelW = L.stripW;
        L.panelH = mt.countH + mt.sectionGap + L.stripH;
    };
    updateExtents();

    // Height fit: two shrinking passes until the panel (count badge + strip)
    // clears the screen margins.
    const double maxPanelH = std::max(1.0, screen.h - mt.screenMargin * 2.0);
    for (int pass = 0; pass < 2 && L.panelH > maxPanelH; ++pass) {
        const double availStripH =
            std::max(1.0, maxPanelH - mt.countH - mt.sectionGap);
        const double hScale = std::min(1.0, availStripH / L.stripH);
        selW *= hScale;
        nearW *= hScale;
        farW *= hScale;
        overlap *= hScale;
        updateExtents();
    }

    L.countX = (L.panelW - L.countW) * 0.5;
    L.stripY = mt.countH + mt.sectionGap;
    L.originX = std::round((screen.w - L.panelW) * 0.5);
    L.originY = std::round((screen.h - L.panelH) * 0.5);

    const int visibleCount = std::min(L.visibleCards, count);
    const int centerSlot = visibleCount / 2;
    const int sel = ((selected % count) + count) % count;
    const int start = ((sel - centerSlot) % count + count) % count;
    const double selX = (L.stripW - selW) * 0.5;
    const double selFrameH = frameH(selW);
    for (int slot = 0; slot < visibleCount; ++slot) {
        const int idx = (start + slot) % count;
        const int rel = slot - centerSlot;
        const int dist = std::abs(rel);
        CarouselLayout::Card& c = L.cards[static_cast<size_t>(idx)];
        c.visible = true;
        c.showCaption = dist == 0;
        c.depth = dist;
        c.direction = rel < 0 ? -1 : (rel > 0 ? 1 : 0);
        c.zIndex = 3 - std::min(dist, 3);
        if (dist == 0) {
            c.w = selW;
            c.x = selX;
        } else if (dist == 1) {
            c.w = nearW;
            c.x = rel < 0 ? selX - nearW + overlap : selX + selW - overlap;
        } else {
            c.w = farW;
            const double nearL = selX - nearW + overlap;
            const double nearR = selX + selW - overlap;
            c.x = rel < 0 ? nearL - farW + overlap : nearR + nearW - overlap;
        }
        // The centered card is the tall one (it carries the caption), so the
        // shorter side cards center their preview against ITS preview box.
        const double h = frameH(c.w);
        c.h = h + (c.showCaption ? mt.captionGap + mt.captionH : 0.0);
        c.y = (selFrameH - h) * 0.5;
    }
    return L;
}

} // namespace gloview
