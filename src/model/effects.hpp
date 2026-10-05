#pragma once

#include "model.hpp"

#include <utility>
#include <vector>

namespace gloview::model {

// Transient visuals with the opposite lifetime to the scene: ghosts of windows
// that just left, the pulse a window kicked off, the flight of a strip thumbnail
// to its new card, and workspaces created this session that must not be reaped
// before close. The scene is REBUILT; these are appended to and pruned.
//
// Pruning is a predicate, not an exposed vector, so "drop this window's effects"
// is one named operation rather than an erase_if lambda per site.
class Effects {
  public:
    [[nodiscard]] const std::vector<Ghost> &ghosts() const { return m_ghosts; }
    [[nodiscard]] const std::vector<WinPulse> &pulses() const {
      return m_pulses;
    }
    [[nodiscard]] const std::vector<SwapFX> &swapfx() const { return m_swapfx; }
    [[nodiscard]] const std::vector<PHLWORKSPACEREF> &newWorkspaces() const {
      return m_newWorkspaces;
    }

    void addGhost(const Ghost &g) { m_ghosts.push_back(g); }
    void addPulse(const WinPulse &p) { m_pulses.push_back(p); }
    void addSwap(SwapFX &&fx) { m_swapfx.push_back(std::move(fx)); }
    void addNewWorkspace(const PHLWORKSPACEREF &ws) {
      m_newWorkspaces.push_back(ws);
    }

    void clearGhosts() { m_ghosts.clear(); }
    void clearNewWorkspaces() { m_newWorkspaces.clear(); }

    // A flight retargets while it runs: the newest is reachable for mutation.
    [[nodiscard]] SwapFX *lastSwap() {
      return m_swapfx.empty() ? nullptr : &m_swapfx.back();
    }

    // The clock is the only place these payloads change.
    template <class F> void eachPulse(F &&f) {
      for (auto &p : m_pulses)
        f(p);
    }
    template <class F> void eachSwap(F &&f) {
      for (auto &fx : m_swapfx)
        f(fx);
    }

    template <class P> void dropPulse(P pred) {
      std::erase_if(m_pulses, pred);
    }
    template <class P> void dropSwap(P pred) {
      std::erase_if(m_swapfx, pred);
    }
  private:
    std::vector<Ghost> m_ghosts;
    std::vector<WinPulse> m_pulses;
    std::vector<SwapFX> m_swapfx;
    std::vector<PHLWORKSPACEREF> m_newWorkspaces;
};

} // namespace gloview::model