#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "model.hpp"

namespace gloview::model {

// The workspace strip: one card per workspace, plus the optional leading "All" card.
//
// This exists because m_strip was the last piece of session state reachable as a bare
// field. Everything else goes through a method -- Scene::tiles(), Effects::ghosts() --
// while m_strip handed out clear/empty/push_back/size from eight translation units, so
// any of them could reorder or inject a card behind the logic that keeps the strip and
// the scene consistent.
//
// Construction is not open on purpose: the strip is rebuilt from scratch by buildStrip()
// and read as a snapshot by the painters, so "append to whatever is there" is never the
// right move and no caller asked for it.
class Strip {
  public:
    [[nodiscard]] const std::vector<StripItem> &items() const { return m_items; }
    [[nodiscard]] std::size_t size() const { return m_items.size(); }
    [[nodiscard]] bool empty() const { return m_items.empty(); }

    // The only mutation the builder needs. Ordering is the builder's business; the
    // strip's own invariants are set by buildStrip() in one place.
    void add(StripItem &&item) { m_items.push_back(std::move(item)); }

    using const_iterator = std::vector<StripItem>::const_iterator;
    [[nodiscard]] const_iterator begin() const { return m_items.begin(); }
    [[nodiscard]] const_iterator end() const { return m_items.end(); }

    // Non-const iteration is how the layout pass assigns each card its box. It mutates
    // geometry only; nothing here may add, remove or reorder a card.
    using iterator = std::vector<StripItem>::iterator;
    [[nodiscard]] iterator begin() { return m_items.begin(); }
    [[nodiscard]] iterator end() { return m_items.end(); }

    [[nodiscard]] const StripItem &operator[](std::size_t i) const {
      return m_items[i];
    }

    // buildStrip() hoists the special cards to the front by moving them out wholesale.
    // Swapping an empty vector in and the leftovers out is the cheapest way to do that
    // without a second container being reallocated every frame.
    [[nodiscard]] std::vector<StripItem> takeAll() {
      return std::exchange(m_items, {});
    }

    void clear() { m_items.clear(); }

  private:
    std::vector<StripItem> m_items;
};

} // namespace gloview::model