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
    return Engine::Rows;
}

// ONE standard box for every card, the size a fullscreen window occupies. Every
// card is that box to the pixel, so the run's geometry cannot depend on which
// window happens to be centred: only its CONTENT differs. A window smaller than
// the box is fitted whole inside it and neither the box nor the window is
// deformed — which is also the only fit that keeps the content's corner arcs off
// the ring, since anything that spills out cannot be rounded back in.
//
// Cards are laid left to right and each one overlaps the previous by `overlap`,
// exactly as the reference's run does; the centred card sits at the middle of the
// run and carries the caption band below it.
CarouselLayout computeCarousel(int count, int selected, const LRect& screen,
                               const CarouselMetrics& mt) {
    CarouselLayout L;
    L.area = screen;
    if (count <= 0)
        return L;
    L.cards.resize(static_cast<size_t>(count));
    L.countW = mt.countW;
    L.countH = mt.countH;

    // Even counts are allowed: centerSlot leaves the selected card one slot off
    // the exact middle, as in the reference.
    const int want = std::max(1, mt.visibleCards);
    const int shown = std::min(want, count);
    L.visibleCards = shown;

    const int centerSlot = shown / 2;
    const int leftSlots = centerSlot;
    const int rightSlots = shown - 1 - centerSlot;

    const int sel = ((selected % count) + count) % count;
    std::vector<int> slotOf(static_cast<size_t>(count), 99);
    std::vector<int> idxOfSlot;
    idxOfSlot.reserve(static_cast<size_t>(shown));
    for (int rel = -leftSlots; rel <= rightSlots; ++rel) {
        const int idx = ((sel + rel) % count + count) % count;
        slotOf[static_cast<size_t>(idx)] = rel;
        idxOfSlot.push_back(idx);
    }

    const double boxW = std::max(1.0, mt.boxW);
    const double boxH = std::max(1.0, mt.boxH);
    const double stripW = boxW * static_cast<double>(shown) -
                          mt.overlap * static_cast<double>(shown - 1);
    // The caption hangs below the centred card, so the panel reserves room for
    // it; the cards themselves stay the standard box.
    const double stripH = boxH + mt.captionGap + mt.captionH;

    // One uniform scale so the run fits both axes at once.
    const double availW = std::max(1.0, screen.w - mt.screenMargin * 2.0);
    const double availH = std::max(1.0, screen.h - mt.screenMargin * 2.0 -
                                             mt.countH - mt.sectionGap);
    const double k = std::min({1.0, availW / std::max(1.0, stripW),
                               availH / std::max(1.0, stripH)});

    const double w = boxW * k;
    const double h = boxH * k;
    const double overlap = mt.overlap * k;

    L.stripW = stripW * k;
    L.stripH = stripH * k;
    L.panelW = L.stripW;
    L.panelH = mt.countH + mt.sectionGap + L.stripH;
    L.countX = (L.panelW - L.countW) * 0.5;
    L.stripY = mt.countH + mt.sectionGap;
    L.originX = std::round((screen.w - L.panelW) * 0.5);
    L.originY = std::round((screen.h - L.panelH) * 0.5);

    for (const int idx : idxOfSlot) {
        const int slot = slotOf[static_cast<size_t>(idx)];
        CarouselLayout::Card& c = L.cards[static_cast<size_t>(idx)];
        c.visible = true;
        c.showCaption = slot == 0;
        c.depth = std::abs(slot);
        // Anchored to the RUN, not to the centred card: centring the card puts
        // the whole run on one side whenever the card count is even, and that is
        // what made the run drift off-centre. originX then centres the run, and
        // since every box is identical the geometry cannot depend on which
        // window is selected at all.
        c.x = static_cast<double>(slot + leftSlots) * (w - overlap);
        // Every box is the standard box, the centred one included: the caption
        // strip is reserved in stripH for the panel's budget, not bolted onto
        // one card, because a taller centred box is a different size.
        c.w = w;
        c.h = h;
        c.y = 0.0;
    }
    return L;
}

} // namespace gloview
