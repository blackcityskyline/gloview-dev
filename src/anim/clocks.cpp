#include <algorithm>
#include <chrono>

#include "../anim/curves.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"

namespace gloview {

namespace {

// Tight cascade window shared by the position glide and the appear stagger:
// a loose per-tile fan reads as a scatter of individually-arriving tiles;
// capped low so the grid moves as one group with just enough depth hint.
double staggered(double base, int i, int n) {
  if (n <= 1)
    return base;
  const double spread = std::min(0.08, 0.015 * n);
  const double start  = spread * (static_cast<double>(i) / (n - 1));
  return std::clamp((base - start) / std::max(0.001, 1.0 - spread), 0.0, 1.0);
}

} // namespace

// ---- Clocks -----------------------------------------------------------------
// The only domain that touches time. Everything here is pure linear progress;
// shaping happens once, in the curve registry. Durations are re-read from
// config on every call so changes reach in-flight animations — a Tween owns
// nothing but its start point (plus the stall guard in updateAnimation:
// wall-time inside a render hole never counts).

// Resolved animation group: {on, ms >= 16, curve}. Resolution order per field:
// imperative rule (gloview.animation{...}) > schema key > built-in default.
// Master off -> 1ms (instant); speed is a multiplier over the leaf's base
// (2.0 = twice as fast), clamped to a one-frame floor.
anim::AnimCfg Overview::leaf(const char *name) const {
  anim::AnimCfg a;
  const auto *spec = cfg::animLeaf(name);
  const auto *rule = cfg::animRule(name);
  double speed = 1.0;
  int absMs = -1;
  a.on = cfg::anim::enabled != 0;
  if (spec && spec->leaf->enabled == 0)
    a.on = false;
  if (rule) {
    if (rule->enabled)
      a.on = *rule->enabled;
    if (rule->speed)
      speed = *rule->speed;
    if (rule->ms)
      absMs = *rule->ms;
    if (rule->curve)
      a.curve = *rule->curve;
  }
  if (!a.on) {
    a.ms = 1.0;
    return a;
  }
  if (absMs >= 0)
    a.ms = std::max(16.0, static_cast<double>(absMs));
  else
    a.ms = std::max(16.0,
                    static_cast<double>(spec ? spec->base : 250.0F) /
                        std::clamp(speed, 0.01, 20.0));
  if (!rule || !rule->curve)
    a.curve = spec ? spec->leaf->curve.get() : std::string("easeout");
  return a;
}

// Which config group drives each side of a rebuild transition. Precedence:
// the all<->one flip owns both halves (expo_in/expo_out), a workspace switch
// uses its enter/exit groups, everything else falls back to plain appear.
const char *Overview::entryLeaf() const {
  if (m_expoFlip != 0)
    return m_expoFlip > 0 ? "expo_in" : "expo_out";
  return m_wsSlideDir != 0 ? "ws_enter" : "appear";
}

const char *Overview::ghostLeaf() const {
  if (m_expoFlip != 0)
    return m_expoFlip > 0 ? "expo_in" : "expo_out";
  return m_wsSlideDir != 0 ? "ws_exit" : "appear";
}

const char *Overview::glideLeaf() const {
  if (m_jumpMode) return m_opening ? "jump_in" : "jump_out";
  return m_expoFlip > 0 ? "expo_in" : m_expoFlip < 0 ? "expo_out" : "glide";
}

double Overview::eased() const {
  // Chrome reveal/collapse follows its own leaf: open while entering, close
  // while exiting (m_progress is the LINEAR clock value either way).
  // Jump mode uses jump_out for the close glide so it can have its own feel.
  const char *leaf_name = (m_jumpMode && !m_opening) ? "jump_out"
                        : m_opening ? "open" : "close";
  return curves::eval(leaf(leaf_name).curve, m_progress);
}

double Overview::animDuration() const {
  if (m_jumpMode && !m_opening) return leaf("jump_out").ms;
  return leaf(m_opening ? "open" : "close").ms;
}

double Overview::tileProgress(int i) const {
  // Slot glides ride the tiles' forward clock (m_tileClock). CLOSE
  // deliberately goes back to riding m_progress DOWN: the tile lerp then has
  // the exact same shape as the collapsing chrome, keeping landing and
  // strip-collapse frame-synced.
  // Use the snapshotted duration (m_tileClockMs) while a glide is in flight
  // so the progress doesn't jump when glideLeaf() changes (e.g. expoFlip
  // clears mid-flight and m_glide switches from expo_in → glide).
  const double glideMs = (m_tileClockMs > 0) ? m_tileClockMs : m_glide.ms;
  const double base = m_opening ? m_tileClock.raw(glideMs) : m_progress;
  return staggered(base, i, static_cast<int>(m_tiles.size()));
}

double Overview::tileAppear(int i) const {
  return curves::eval(
      m_entry.curve,
      staggered(m_rebuildClock.raw(m_entry.ms), i,
                static_cast<int>(m_tiles.size())));
}

LRect Overview::currentBox(const model::Tile &t, int i) const {
  const std::string &curve = m_tileClockCurve.empty() ? m_glide.curve : m_tileClockCurve;
  const double e = curves::eval(curve, tileProgress(i));
  const auto &a = t.natural;
  const auto &b = t.target;
  LRect r{lerp(a.x, b.x, e), lerp(a.y, b.y, e), lerp(a.w, b.w, e),
          lerp(a.h, b.h, e)};
  const double ap = t.appear < 1.0 ? tileAppear(i) : 1.0;
  if (ap >= 1.0)
    return r;
  // Entry style on any rebuild transition that carries a direction (ws
  // switches and the all<->one flip, dir fixed +1 there): slide arrives from
  // the ws-id-order side a full monitor width out; slidevert drops from the
  // top edge — mirrors of the ghost exits in renderGhosts; fade is a pure
  // alpha ramp (entryFade), no geometry; anything else pops from the center.
  if (m_wsSlideDir != 0 && m_enterStyle != "pop") {
    if (const auto m = m_monitor.lock()) {
      if (m_enterStyle == "slide")
        r.x += static_cast<double>(m_wsSlideDir) * m->m_size.x * (1.0 - ap);
      else if (m_enterStyle == "slidevert")
        r.y -= static_cast<double>(m->m_size.y) * (1.0 - ap);
    }
    if (m_enterStyle == "slide" || m_enterStyle == "slidevert" ||
        m_enterStyle == "fade")
      return r;
  }
  const double k  = 0.85 + 0.15 * ap;
  const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
  return LRect{cx - r.w * k / 2.0, cy - r.h * k / 2.0, r.w * k, r.h * k};
}

void Overview::updateAnimation() {
  // Resolve this frame's leaves ONCE; paint (per-tile) reads the caches.
  m_glide      = leaf(glideLeaf());
  m_entry      = leaf(entryLeaf());
  m_ghost      = leaf(ghostLeaf());
  m_lift       = leaf("drag");
  m_enterStyle = cfg::animStyle("ws_enter");
  m_exitStyle  = cfg::animStyle("ws_exit");

  const double dur = animDuration();
  // Stall guard: a render hole rewinds EVERY clock to its last-known value so
  // wall-time inside the hole never counts.
  const auto now = std::chrono::steady_clock::now();
  if (m_lastAnimTick.time_since_epoch().count() != 0) {
    const double gapMs =
        std::chrono::duration<double, std::milli>(now - m_lastAnimTick).count();
    if (gapMs > 100.0) {
      const double glideMs = (m_tileClockMs > 0) ? m_tileClockMs : m_glide.ms;
      m_timeline.compensateStall(gapMs, dur);
      m_tileClock.compensateStall(gapMs, glideMs);
      m_rebuildClock.compensateStall(gapMs, std::max(m_entry.ms, m_ghost.ms));
      m_stripTween.compensateStall(gapMs, leaf("strip_step").ms);
      if (m_newCardAnim)
        m_newCard.compensateStall(gapMs, newCardDur());
      if (m_drag.lifted)
        m_dragLiftClock.compensateStall(gapMs, leaf("drag").ms);
      for (auto &fx : m_swapfx)
        fx.clock.compensateStall(gapMs, fx.ms);
    }
  }
  m_lastAnimTick = now;

  // Hold timer: promote armed-but-not-lifted press to lift after hold_lift_ms.
  if (m_drag.armed() && !m_drag.lifted) {
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        now - m_drag.holdStartMs).count();
    if (elapsedMs >= static_cast<double>(cfg::behavior.hold_lift_ms.get())) {
      m_drag.lifted = true;
      m_dragLiftClock.begin();
      damage();
    } else {
      // Stale-repaint: ensure the overlay redraws periodically while
      // armed-but-not-lifted so the hold timer can tick even without input.
      static auto lastHoldDamage = std::chrono::steady_clock::time_point{};
      const auto sinceLast = std::chrono::duration<double, std::milli>(
          now - lastHoldDamage).count();
      if (sinceLast >= 50.0) {
        damage();
        lastHoldDamage = now;
      }
    }
  }

  const auto st = leaf("strip_step");
  if (!m_stripTween.done(st.ms))
    m_stripScroll =
        lerp(m_stripScrollFrom, m_stripScrollTarget,
             curves::eval(st.curve, m_stripTween.raw(st.ms)));
  else
    m_stripScroll = m_stripScrollTarget;

  // Each side of the transition closes in its own window...
  if (!m_ghosts.empty() && m_rebuildClock.done(m_ghost.ms))
    m_ghosts.clear();
  // ...and entries settle explicitly, or every newcomer tile keeps resolving
  // config and evaluating curves forever after.
  if (m_rebuildClock.done(m_entry.ms) &&
      std::any_of(m_tiles.begin(), m_tiles.end(),
                  [](const model::Tile &t) { return t.appear < 1.0; }))
    for (auto &t : m_tiles)
      t.appear = 1.0;

  // Swap pulses accumulate per animated frame with the delta CAPPED, so a
  // post-drop render hole cannot jump the ring through its overshoot plateau.
  const auto pulse = leaf("pulse");
  for (auto &p : m_pulses) {
    if (p.w.expired())
      continue;
    p.p += std::min(34.0, std::chrono::duration<double, std::milli>(now - p.last)
                              .count()) /
           pulse.ms;
    p.last = now;
  }
  std::erase_if(m_pulses, [](const model::WinPulse &p) {
    return p.w.expired() || p.p >= 1.0;
  });

  // Transition over only when BOTH sides finished (flags must outlive the
  // caches computed from them).
  if (m_rebuildClock.done(m_ghost.ms) && m_rebuildClock.done(m_entry.ms)) {
    m_wsSlideDir = 0;
    m_expoFlip = 0;
  }

  // Glide done: clear the snapshotted duration so glideDur() reads live
  // config again on the next startTileGlide() call.
  if (m_tileClockMs > 0 && m_tileClock.done(m_tileClockMs)) {
    m_tileClockMs = 0;
    m_tileClockCurve.clear();
  }

  // Done flights (and vanished windows) leave the Model by their OWN clock —
  // pruning against live config cut mid-flight landings on a config change.
  for (auto it = m_swapfx.begin(); it != m_swapfx.end();) {
    if (it->win.expired() || it->clock.raw(it->ms) >= 1.0)
      it = m_swapfx.erase(it);
    else
      ++it;
  }

  const double t = m_timeline.raw(dur);
  m_progress = m_opening ? t : 1.0 - t;
  if (m_newCardAnim && m_newCard.done(newCardDur())) {
    m_newCardAnim = false;
    m_newCardId = 0;
  }
  // Close completion waits for the timeline AND any tile clock still draining
  // (a reflow begun during close would otherwise pop its landing tiles).
  if (!m_opening && t >= 1.0 && m_tileClock.done(dur)) {
    m_progress = 0.0;
    m_pendingDeactivate = true;
  }
}

// The single pump predicate: anything that can still produce motion. Every
// site (paint tail, pump arm, pump tick) asks this one function — a new clock
// registers here exactly once.
bool Overview::animBusy() const {
  return m_active &&
         (secondaryAnimsActive() || !m_tileClock.done(glideDur()) ||
          m_newCardAnim || m_drag.lifted || !m_swapfx.empty() ||
          (m_opening && m_progress < 1.0) || (!m_opening && m_progress > 0.0));
}

double Overview::newCardScale() const {
  if (!m_newCardAnim)
    return 1.0;
  return curves::eval(leaf("card").curve, m_newCard.raw(newCardDur()));
}

void Overview::animateStripTo(double from, double to) {
  from = std::clamp(from, 0.0, std::max(0.0, m_stripScrollMax));
  to   = std::clamp(to, 0.0, std::max(0.0, m_stripScrollMax));
  const auto st = leaf("strip_step");
  if (!st.on || std::abs(to - from) < 0.5) {
    m_stripScrollFrom = m_stripScrollTarget = m_stripScroll = to;
    return;
  }
  // Retarget mid-flight from wherever the eased value is NOW so rapid wheel
  // notches read as one continuous scrub.
  m_stripScrollFrom = !m_stripTween.done(st.ms) ? m_stripScroll : from;
  m_stripScrollTarget = to;
  m_stripTween.begin();
  ensureAnimPump();
}

} // namespace gloview
