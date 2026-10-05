#include "recency.hpp"

#include <algorithm>
#include <limits>

namespace gloview::model {

void Recency::promote(const void *window) {
  if (!window)
    return;
  const auto it = std::find(m_front.begin(), m_front.end(), window);
  if (it == m_front.end()) {
    m_front.push_front(window);
    return;
  }
  if (it != m_front.begin())
    std::rotate(m_front.begin(), it, it + 1);
}

bool Recency::ranked(const void *window) const {
  return std::find(m_front.begin(), m_front.end(), window) != m_front.end();
}

std::size_t Recency::rank(const void *window) const {
  const auto it = std::find(m_front.begin(), m_front.end(), window);
  return it == m_front.end() ? std::numeric_limits<std::size_t>::max()
                            : static_cast<std::size_t>(
                                  std::distance(m_front.begin(), it));
}

void Recency::order(const std::vector<const void *> &keys,
                    std::vector<std::size_t> &out) const {
  out.resize(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i)
    out[i] = i;
  // Rank ascending; stable, so keys with the same rank (all the unranked ones)
  // stay in the order the caller gave.
  std::stable_sort(out.begin(), out.end(),
                   [&](const std::size_t a, const std::size_t b) {
                     return rank(keys[a]) < rank(keys[b]);
                   });
}

void Recency::forgetDead(const std::vector<const void *> &live) {
  std::erase_if(m_front, [&](const void *k) {
    return std::find(live.begin(), live.end(), k) == live.end();
  });
}

} // namespace gloview::model