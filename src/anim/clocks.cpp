#include <algorithm>
#include <chrono>
#include <cmath>

#include "../overview.hpp"

namespace gloview {

namespace {
double lerp(double a, double b, double t) { return a + (b - a) * t; }
} // namespace

// ---- animation clocks -------------------------------------------------------
// The Clocks store: every value here is PURE TIME — linear progress shaped by
// the animation registry's curves. Nothing in this file touches pixels, and
// nothing in the painter shapes time itself: the painter reads the values
// these functions produce. Durations are re-read from config on every call so
// changes apply to in-flight animations; the Tween only owns its start point
// (plus stall compensation — wall-time inside a render hole never counts).

double Overview::eased() const {
  // Chrome reveal/collapse curve follows its own leaf: open while entering,
  // close while exiting (m_progress is the LINEAR clock value either way).
  return curveEval(anim(m_opening ? "open" : "close").curve, m_progress);
}

double Overview::animDuration() const {
  // Legacy shared knob: open/close/reflow leaves follow it when their own
  // _ms is unset (sentinel -1). Master-off collapses everything to 1ms.
  return animMs(m_opening ? "open" : "close",
                "plugin:gloview:duration", 360);
}

double Overview::tileProgress(int i) const {
  // Entry and reflows ride the tiles' own forward clock (m_tileClock). CLOSE
  // deliberately goes back to riding m_progress DOWN: the tile lerp then has
  // the exact same shape as the collapsing chrome (both eased of the
  // descending progress), keeping landing and strip-collapse frame-synced.
  const double base =
      m_opening ? m_tileClock.raw(reflowDur()) : m_progress;
  const int n = static_cast<int>(m_tiles.size());
  if (n <= 1)
    return base;
  // Cascade window, deliberately tight: a loose per-tile fan reads as a
  // scatter of individually-arriving tiles rather than one cohesive motion.
  // Capped low so the grid moves as a single group with just enough offset
  // left to hint at depth/order.
  const double spread = std::min(0.08, 0.015 * n); // total cascade window
  const double start  = spread * (static_cast<double>(i) / (n - 1));
  const double span   = std::max(0.001, 1.0 - spread);
  return std::clamp((base - start) / span, 0.0, 1.0);
}

double Overview::tileAppear(int i) const {
  // Same tight stagger as the position glide, on the populate clock.
  const int n = static_cast<int>(m_tiles.size());
  if (n <= 1)
    return curveEval(anim("populate").curve,
                     m_populate.raw(populateMs()));
  const double base  = m_populate.raw(populateMs());
  const double spread = std::min(0.08, 0.015 * n);
  const double start  = spread * (static_cast<double>(i) / (n - 1));
  const double span   = std::max(0.001, 1.0 - spread);
  return curveEval(anim("populate").curve,
                   std::clamp((base - start) / span, 0.0, 1.0));
}

LRect Overview::currentBox(const Tile &t, int i) const {
  // Plain smooth deceleration: easeOutBack's per-tile bounce landed at
  // visibly different moments and read as jerky; one shared curve with no
  // overshoot reads as "monolithic".
  const double e = curveEval(anim("reflow").curve, tileProgress(i));
  const auto &a = t.natural;
  const auto &b = t.target;
  LRect r{lerp(a.x, b.x, e), lerp(a.y, b.y, e), lerp(a.w, b.w, e),
          lerp(a.h, b.h, e)};
  // population scale: a brand-new tile grows from its slot center
  const double ap = t.appear < 1.0 ? tileAppear(i) : 1.0;
  if (ap < 1.0) {
    const double k  = 0.85 + 0.15 * ap;
    const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    r = LRect{cx - r.w * k / 2.0, cy - r.h * k / 2.0, r.w * k, r.h * k};
  }
  return r;
}

void Overview::updateAnimation() {
  const double dur = animDuration();
  // Stall guard FIRST: measure the gap since the previous animated frame; a
  // render hole rewinds every active clock to its last-known position so the
  // wall-time inside the hole never counts.
  const auto nowTick = std::chrono::steady_clock::now();
  if (m_lastAnimTick.time_since_epoch().count() != 0) {
    const double gapMs =
        std::chrono::duration<double, std::milli>(nowTick - m_lastAnimTick)
            .count();
    if (gapMs > 100.0) {
      m_timeline.compensateStall(gapMs, dur);
      m_tileClock.compensateStall(gapMs, reflowDur());
      if (m_newCardAnim)
        m_newCard.compensateStall(gapMs, newCardDur());
    }
  }
  m_lastAnimTick = nowTick;

  // AN5: ease the strip scroll toward its target (strip_step leaf).
  if (!m_stripTween.done(animMs("strip_step", nullptr, 200)))
    m_stripScroll = std::lerp(
        m_stripScrollFrom, m_stripScrollTarget,
        curveEval(anim("strip_step").curve,
                  m_stripTween.raw(animMs("strip_step", nullptr, 200))));
  else
    m_stripScroll = m_stripScrollTarget;

  if (m_populate.done(populateMs()) && !m_ghosts.empty())
    m_ghosts.clear();

  // Advance/prune swap pulses. Progress accumulates per animated frame with
  // the frame delta CAPPED, so a post-drop render hole cannot jump the ring
  // through its overshoot plateau (the "snaps wide and freezes" artifact).
  const double pulseMs = animMs("swap_pulse", nullptr, 180);
  const auto nowTickP  = std::chrono::steady_clock::now();
  for (auto &p : m_pulses) {
    if (p.w.expired())
      continue;
    const double dt =
        std::min(34.0,
                 std::chrono::duration<double, std::milli>(nowTickP - p.last)
                     .count());
    p.last = nowTickP;
    p.p += dt / std::max(1.0, pulseMs);
  }
  std::erase_if(m_pulses,
                [](const WinPulse &p) { return p.w.expired() || p.p >= 1.0; });

  const double t = m_timeline.raw(dur);
  m_progress = m_opening ? t : 1.0 - t;
  if (m_newCardAnim && m_newCard.done(newCardDur())) {
    m_newCardAnim = false;
    m_newCardId = 0;
  }
  // Close completion needs BOTH clocks done: chrome finishes early, but
  // flipping anything off before the tiles have landed would pop them
  // mid-air. Pin progress to 0 and flag the teardown; it runs from the
  // painter's tail (finishPendingDeactivate) after the final frame painted.
  if (!m_opening && t >= 1.0 && m_tileClock.done(dur)) {
    m_progress = 0.0;
    m_pendingDeactivate = true;
  }
}

double Overview::newCardScale() const {
  if (!m_newCardAnim)
    return 1.0;
  const double p = m_newCard.raw(newCardDur());
  // pop curve from the registry ("back" default — a little overshoot)
  return curveEval(anim("new_card").curve, p);
}

void Overview::animateStripTo(double from, double to) {
  from = std::clamp(from, 0.0, std::max(0.0, m_stripScrollMax));
  to   = std::clamp(to, 0.0, std::max(0.0, m_stripScrollMax));
  const auto a = anim("strip_step");
  if (!a.on || std::abs(to - from) < 0.5) {
    m_stripScrollFrom   = to;
    m_stripScrollTarget = to;
    m_stripScroll       = to;
    return;
  }
  // retarget mid-flight: continue from wherever the eased value is NOW so
  // rapid wheel notches read as one continuous scrub
  m_stripScrollFrom =
      !m_stripTween.done(a.ms) ? m_stripScroll : from;
  m_stripScrollTarget = to;
  m_stripTween.begin();
  ensureAnimPump();
}

} // namespace gloview
