#pragma once

#include "../layout.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace gloview::view {

// Which view is on screen, which way it is moving, the monitor-to-preview
// transform, and what layoutTiles last resolved.
//
// The resolved layout lives here because the renderer must ask what was produced,
// never the config: free-arrange, the all/one flip and the carousel switch each
// override what `layout` says. Keeping it with the selection it was resolved for
// makes that one question of one object; it was four fields compared by hand.
class State {
  public:
    [[nodiscard]] bool desktop() const { return m_desktop; }
    void setDesktop(bool on) { m_desktop = on; }

    [[nodiscard]] int allOverride() const { return m_allOverride; }
    void setAllOverride(int v) { m_allOverride = v; }

    [[nodiscard]] bool run() const { return m_run; }
    void setRun(bool on) { m_run = on; }

    [[nodiscard]] bool carouselOn() const { return m_carouselOn; }
    void setCarouselOn(bool on) { m_carouselOn = on; }

    [[nodiscard]] int wsSlideDir() const { return m_wsSlideDir; }
    void setWsSlideDir(int d) { m_wsSlideDir = d; }

    [[nodiscard]] int expoFlip() const { return m_expoFlip; }
    void setExpoFlip(int d) { m_expoFlip = d; }

    [[nodiscard]] bool jumpMode() const { return m_jumpMode; }
    void setJumpMode(bool on) { m_jumpMode = on; }

    [[nodiscard]] double scale() const { return m_s; }
    [[nodiscard]] double ox() const { return m_ox; }
    [[nodiscard]] double oy() const { return m_oy; }
    void setTransform(double s, double ox, double oy) {
      m_s = s;
      m_ox = ox;
      m_oy = oy;
    }

    [[nodiscard]] Engine engine() const { return m_engine; }
    [[nodiscard]] const CarouselLayout &carousel() const { return m_carousel; }
    [[nodiscard]] int carouselFor() const { return m_carouselFor; }

    // slot[k] = the card index that card slot k shows. The renderer needs this too,
    // so it belongs with the geometry it was resolved with: card k is NOT tile k,
    // and a renderer assuming otherwise draws the wrong window in every slot.
    [[nodiscard]] const std::vector<std::size_t> &slot() const { return m_slot; }


    [[nodiscard]] int slotOf(std::size_t tile) const {
      for (std::size_t k = 0; k < m_slot.size(); ++k)
        if (m_slot[k] == tile)
          return static_cast<int>(k);
      return -1;
    }

    void setEngine(Engine e) { m_engine = e; }
    void clearCards() {
      m_carousel = {};
      m_carouselFor = -1;
      m_slot.clear();
    }
    void resolve(Engine engine, const CarouselLayout &carousel, int selected,
                 std::vector<std::size_t> slot) {
      m_engine = engine;
      m_carousel = carousel;
      m_carouselFor = selected;
      m_slot = std::move(slot);
    }
    void clearRun() {
      m_engine = Engine::Rows;
      clearCards();
    }
    void invalidateRun() { m_carouselFor = -1; }

  private:
    bool m_desktop = false;
    int  m_allOverride = -1;
    bool m_run = false;
    bool m_carouselOn = true;

    int  m_wsSlideDir = 0;
    int  m_expoFlip = 0;
    bool m_jumpMode = false;

    double m_s = 1.0, m_ox = 0.0, m_oy = 0.0;

    Engine m_engine = Engine::Rows;
    CarouselLayout m_carousel;
    std::vector<std::size_t> m_slot;
    int m_carouselFor = -1;
};

} // namespace gloview::view