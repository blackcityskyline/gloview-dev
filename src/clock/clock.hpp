#pragma once

#include "clocks.hpp"

#include <string>

namespace gloview::clock {

// One animation clock. Wraps anim::Tween so that a caller can start, seek and
// query it, but cannot reach the raw anchor and last-sample state — the only way
// to move time in a plugin whose animations are re-targeted mid-flight is through
// these four verbs.
class Clock {
  public:
    void begin() { m_t.begin(); }
    // Drops the anchor entirely, so raw() reads 0 again. Assigning a fresh Tween
    // used to be the only way to say this, which needed the internals public.
    void reset() { m_t = anim::Tween{}; }
    void seek(double frac, double durMs) { m_t.seek(frac, durMs); }
    void pinEnd(double durMs) { m_t.pinEnd(durMs); }
    bool done(double durMs) const { return m_t.done(durMs); }
    double raw(double durMs) const { return m_t.raw(durMs); }
    // Rewinds to the last sampled fraction when the frame stalled, so a hitch
    // does not teleport the animation forward.
    void compensateStall(double gapMs, double durMs) {
      m_t.compensateStall(gapMs, durMs);
    }

  private:
    anim::Tween m_t;
};

// The domain of time. Every tween in the plugin lives here and nowhere else, so
// "is anything still moving" is one question asked of one object rather than a
// tally assembled at each of a dozen call sites — the pump predicates, the busy
// test and the painter's per-frame reads all had to know the full inventory to
// answer it, which is exactly how one of them drifted out of date.
//
// Read-only during paint: paint asks what time it is, it never advances it.
class Clocks {
  public:
    // Open/close. master() runs 0 at the start of a close and 1 when settled, so
    // every exit fraction derives from one named value rather than inverting
    // progress at each site.
    Clock master;
    Clock tiles;    // the glide every tile rides; retargeted on any layout change
    Clock rebuild;  // entries and ghost exits share this one
    Clock strip;    // animated card-group offset
    Clock dragLift; // a dragged tile's raise
    Clock newCard;  // a freshly added workspace card

    // Snapshot of the tile glide's recipe, taken when it starts: a config edit
    // mid-flight must not change the duration of an animation already running.
    double tileMs = 0.0;
    std::string tileCurve;

    // The SECONDARY clocks only. The master open/close clock is not in here: a
    // secondary is one that finishes after the master does, and a predicate that
    // folded the master in would report busy during every close, forever.
    [[nodiscard]] bool anyRunning(double rebuildEntryMs, double rebuildGhostMs,
                                  double stripMs) const;
};

} // namespace gloview::clock