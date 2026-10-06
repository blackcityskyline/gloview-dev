#include "clock.hpp"

namespace gloview::clock {

bool Clocks::anyRunning(double rebuildEntryMs, double rebuildGhostMs,
                        double stripMs, double blurFadeMs) const {
  return !rebuild.done(rebuildEntryMs) || !rebuild.done(rebuildGhostMs) ||
         !strip.done(stripMs) || !blurFade.done(blurFadeMs);
}

} // namespace gloview::clock