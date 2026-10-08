#pragma once

#include <cstddef>
#include <deque>
#include <vector>

namespace gloview::model {

// Recency order over windows, most recent first.
//
// NOT owned by the carousel: the run, the strip and alt-tab all want this order,
// and a tracker inside one of them means the next consumer re-feeds its own copy.
// Pure on purpose — addresses in, indices out, no compositor types, no view — so
// it is testable and any feature can both feed and query it.
//
// promote(), from the reference switcher: dead entries dropped first; a new window
// to the front; an already-ranked one ROTATED there, not erased and reinserted.
// Unranked sorts after everything, which falls out of rank() returning max.
class Recency {
  public:
    void promote(const void *window);

    [[nodiscard]] bool ranked(const void *window) const;
    [[nodiscard]] std::size_t rank(const void *window) const;

    // Indices of `keys` into `out`, most-recent first; resized to keys.size().
    // Stable among equals, so unranked windows keep the caller's order.
    void order(const std::vector<const void *> &keys,
               std::vector<std::size_t> &out) const;

    // Drops entries not in `live`: recency is a cache of the live set.
    void forgetDead(const std::vector<const void *> &live);

    void clear() { m_front.clear(); }
    [[nodiscard]] bool empty() const { return m_front.empty(); }
    [[nodiscard]] std::size_t size() const { return m_front.size(); }

  private:
    // Front is the most recent. Addresses, like the reference.
    std::deque<const void *> m_front;
};

} // namespace gloview::model