#pragma once

#include "model.hpp"

#include <vector>

namespace gloview::model {

// The cards in the main area, which one is focused, what the cursor is over.
//
// edit() is the only way to reach the tiles for WRITING. Greppable: six files —
// the build paths, the close pinning, the swap, the desktop park, teardown, and
// the clock pass that settles a finished entry. Every renderer and input handler
// reads tiles() and cannot reorder the array. Recency ordering went wrong once by
// sorting this array inside a layout pass, so the rule lives in the type: a reader
// that wants to reorder has to type edit().
//
// That distinction is the whole point. The recency ordering went wrong once by
// being applied to the array inside a layout pass, because layout and storage were
// the same field. Ordering belongs to the slot layer; storage belongs here.
class Scene {
  public:
    [[nodiscard]] const std::vector<Tile> &tiles() const { return m_tiles; }
    [[nodiscard]] std::size_t size() const { return m_tiles.size(); }
    [[nodiscard]] bool empty() const { return m_tiles.empty(); }


    [[nodiscard]] std::vector<Tile> &edit() { return m_tiles; }
    void clear() { m_tiles.clear(); }

    [[nodiscard]] int selected() const { return m_selected; }
    [[nodiscard]] int hoveredTile() const { return m_hoveredTile; }
    [[nodiscard]] int hoveredStrip() const { return m_hoveredStrip; }

    // Selection is an index every reader assumed was in range.
    [[nodiscard]] bool hasSelection() const {
      return m_selected >= 0 &&
             static_cast<std::size_t>(m_selected) < m_tiles.size();
    }
    void select(int i) { m_selected = i; }

    void stepSelection(int dir) {
      if (m_tiles.empty())
        return;
      const int n = static_cast<int>(m_tiles.size());
      m_selected = ((m_selected < 0 ? 0 : m_selected) + dir % n + n) % n;
    }

    void hover(int tile, int strip) {
      m_hoveredTile = tile;
      m_hoveredStrip = strip;
    }
    void clearHover() {
      m_hoveredTile = -1;
      m_hoveredStrip = -1;
    }

  private:
    std::vector<Tile> m_tiles;
    int m_selected = -1;
    int m_hoveredTile = -1;
    int m_hoveredStrip = -1;
};

} // namespace gloview::model