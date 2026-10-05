#pragma once

#include "../layout.hpp"

namespace gloview::view {

// Which view is on screen, which way it is moving, the monitor-to-preview
// transform, and what layoutTiles last resolved.
//
// The resolved layout lives here because the renderer must ask what was produced,
// never the config: free-arrange, the all/one flip and the carousel switch each
// override what `layout` says. Keeping it together with the selection it was
// resolved for makes that one question of one object — it used to be three fields
// in a header, and the renderer compared them by hand.
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

    // --- what layoutTiles resolved ------------------------------------
    [[nodiscard]] Engine engine() const { return m_engine; }
    [[nodiscard]] const CarouselLayout &carousel() const { return m_carousel; }
    [[nodiscard]] int carouselFor() const { return m_carouselFor; }

    // One call, so "resolved for THIS selection" cannot be half true: slots are
    // built around the selection, and a stale value skips the relayout.
    void setEngine(Engine e) { m_engine = e; }
    // Drops the run's geometry, keeps the already-resolved engine.
    void clearCards() {
      m_carousel = {};
      m_carouselFor = -1;
    }
    void resolve(Engine engine, const CarouselLayout &carousel, int selected) {
      m_engine = engine;
      m_carousel = carousel;
      m_carouselFor = selected;
    }
    void clearRun() {
      m_engine = Engine::Rows;
      clearCards();
    }
    // Marks the run stale; renderStage relayouts on the mismatch.
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
    int m_carouselFor = -1;
};

} // namespace gloview::view