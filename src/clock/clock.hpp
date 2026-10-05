#pragma once

#include "clocks.hpp"

#include <string>

namespace gloview::clock {

// One animation clock: begin, seek, pin, query. The anchor stays inside.
class Clock {
  public:
    void begin() { m_t.begin(); }

    void reset() { m_t = anim::Tween{}; }
    void seek(double frac, double durMs) { m_t.seek(frac, durMs); }
    void pinEnd(double durMs) { m_t.pinEnd(durMs); }
    bool done(double durMs) const { return m_t.done(durMs); }
    double raw(double durMs) const { return m_t.raw(durMs); }

    void compensateStall(double gapMs, double durMs) {
      m_t.compensateStall(gapMs, durMs);
    }

  private:
    anim::Tween m_t;
};

// The domain of time: every tween lives here and nowhere else, so "is anything
// still moving" is one question of one object rather than a tally assembled at
// each call site. Read-only during paint.
class Clocks {
  public:
    Clock master;
    Clock tiles;    // the glide every tile rides; retargeted on any layout change
    Clock rebuild;  // entries and ghost exits share this one
    Clock strip;    // animated card-group offset
    Clock dragLift; // a dragged tile's raise
    Clock newCard;  // a freshly added workspace card

    // The glide's recipe, snapshotted at its start: a config edit mid-flight must
    // not change the duration of an animation already running.
    double tileMs = 0.0;
    std::string tileCurve;

    // The SECONDARY clocks only; folding the master in reports busy forever.
    [[nodiscard]] bool anyRunning(double rebuildEntryMs, double rebuildGhostMs,
                                  double stripMs) const;
};

} // namespace gloview::clock