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
    return Engine::Rows;
}

} // namespace gloview
