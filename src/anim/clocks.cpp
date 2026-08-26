#include <algorithm>
#include <chrono>
#include <cmath>

#include "../anim/curves.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"

namespace gloview {

using AnimCfg = anim::AnimCfg; // return-type lookup for Overview::anim hits class scope first

namespace {
double lerp(double a, double b, double t) { return a + (b - a) * t; }
} // namespace

// ---- animation clocks -------------------------------------------------------
// Leaf resolution (config -> {enabled, ms, curve}) lives here too: the clock
// readers and the registry lookup are one domain.
// The Clocks store: every value here is PURE TIME — linear progress shaped by
// the animation registry's curves. Nothing in this file touches pixels, and
// nothing in the painter shapes time itself: the painter reads the values
// these functions produce. Durations are re-read from config on every call so
// changes apply to in-flight animations; the Tween only owns its start point
// (plus stall compensation — wall-time inside a render hole never counts).

AnimCfg Overview::anim(const char *leaf) const {
  anim::AnimCfg a;
  a.on = cfg::anim.enabled != 0;
  if (const auto *e = cfg::anim.leafEnabled(leaf); e && *e == 0)
    a.on = false;
  if (const auto *ms = cfg::anim.leafMs(leaf))
    a.ms = ms->get(); // -1 = follow the legacy duration knob
  if (const auto *c = cfg::anim.leafCurve(leaf))
    a.curve = c->get();
  return a;
}

// ---- transition leaf selectors ----------------------------------------------
// Which config group drives each side of a populate-clock transition.
// Precedence: the all<->one expo flip owns both halves; a genuine ws switch
// uses its enter/exit groups; everything else falls back to plain populate
// (the pre-group behavior, kept pixel-identical). Both selectors return a
// RESOLVED window — animMs semantics: master/group off → 1ms (instant),
// _ms = -1 sentinel → the legacy duration knob.

namespace {
void resolveLeaf(AnimCfg &a, double fallbackDur) {
  if (!a.on) {
    a.ms = 1.0;
    return;
  }
  if (a.ms < 0)
    a.ms = std::max(1.0, fallbackDur);
}
} // namespace

AnimCfg Overview::entryLeaf() const {
  const bool flip = m_expoFlip != 0, slide = m_wsSlideDir != 0;
  AnimCfg a = flip ? anim(m_expoFlip > 0 ? "expo_in" : "expo_out")
                   : slide ? anim("ws_in") : anim("populate");
  resolveLeaf(a, static_cast<double>(cfg::anim.duration));
  return a;
}

AnimCfg Overview::ghostLeaf() const {
  const bool flip = m_expoFlip != 0, slide = m_wsSlideDir != 0;
  AnimCfg a = flip ? anim(m_expoFlip > 0 ? "expo_in" : "expo_out")
                   : slide ? anim("ws_out") : anim("populate");
  resolveLeaf(a, static_cast<double>(cfg::anim.duration));
  return a;
}

const char *Overview::glideLeaf() const {
  return m_expoFlip > 0 ? "expo_in" : m_expoFlip < 0 ? "expo_out" : "reflow";
}

double Overview::eased() const {
  // Chrome reveal/collapse curve follows its own leaf: open while entering,
  // close while exiting (m_progress is the LINEAR clock value either way).
  return curves::eval(anim(m_opening ? "open" : "close").curve, m_progress);
}

double Overview::animDuration() const {
  // Legacy shared knob: open/close/reflow leaves follow it when their own
  // _ms is unset (sentinel -1). Master-off collapses everything to 1ms.
  return animMs(m_opening ? "open" : "close");
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
  // Same tight stagger as the position glide, on the entry leaf (expo flip >
  // ws switch > populate).
  const auto leaf = entryLeaf();
  const int n = static_cast<int>(m_tiles.size());
  if (n <= 1)
    return curves::eval(leaf.curve, m_populate.raw(leaf.ms));
  const double base  = m_populate.raw(leaf.ms);
  const double spread = std::min(0.08, 0.015 * n);
  const double start = spread * (static_cast<double>(i) / (n - 1));
  const double span   = std::max(0.001, 1.0 - spread);
  return curves::eval(leaf.curve,
                   std::clamp((base - start) / span, 0.0, 1.0));
}

LRect Overview::currentBox(const model::Tile &t, int i) const {
  // Plain smooth deceleration: easeOutBack's per-tile bounce landed at
  // visibly different moments and read as jerky; one shared curve with no
  // overshoot reads as "monolithic". During an expo flip the glide IS the
  // spread/collapse — it reads its own half's curve.
  const double e = curves::eval(anim(glideLeaf()).curve, tileProgress(i));
  const auto &a = t.natural;
  const auto &b = t.target;
  LRect r{lerp(a.x, b.x, e), lerp(a.y, b.y, e), lerp(a.w, b.w, e),
          lerp(a.h, b.h, e)};
  // population scale: a brand-new tile grows from its slot center
  const double ap = t.appear < 1.0 ? tileAppear(i) : 1.0;
  if (ap < 1.0) {
    // Entry styles read ws_enter_anim on ANY populate-clock transition —
    // ws switches AND the all<->one flip (dir is fixed +1 there: new content
    // from the right, per the flip's contract). slide arrives from the
    // ws-id-order side, a full monitor width out; slidevert drops from the
    // top edge — the mirror of the exit paths in renderGhosts.
    const bool styled = m_wsSlideDir != 0;
    const std::string enterAnim = styled ? cfg::anim.ws_enter_anim.get()
                                         : std::string();
    if (enterAnim == "slide" || enterAnim == "slidevert") {
      if (const auto m = m_monitor.lock()) {
        if (enterAnim == "slide")
          r.x += static_cast<double>(m_wsSlideDir) * m->m_size.x * (1.0 - ap);
        else
          r.y -= static_cast<double>(m->m_size.y) * (1.0 - ap);
      }
      return r;
    }
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
  if (!m_stripTween.done(animMs("strip_step")))
    m_stripScroll = std::lerp(
        m_stripScrollFrom, m_stripScrollTarget,
        curves::eval(anim("strip_step").curve,
                  m_stripTween.raw(animMs("strip_step"))));
  else
    m_stripScroll = m_stripScrollTarget;

  // Ghosts ride their own leaf's window (expo_out/ws_out may differ from
  // populate's) — clearing them on populateMs() cut long exits mid-flight.
  if (m_populate.done(ghostLeaf().ms) && !m_ghosts.empty())
    m_ghosts.clear();

  // Advance/prune swap pulses. Progress accumulates per animated frame with
  // the frame delta CAPPED, so a post-drop render hole cannot jump the ring
  // through its overshoot plateau (the "snaps wide and freezes" artifact).
  const double pulseMs = animMs("swap_pulse");
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
                [](const model::WinPulse &p) { return p.w.expired() || p.p >= 1.0; });

  // Transition over only when BOTH sides finished (each on its own leaf's
  // window). Compute the leaves BEFORE clearing the state they read.
  if (m_populate.done(ghostLeaf().ms) && m_populate.done(entryLeaf().ms)) {
    m_wsSlideDir = 0;
    m_expoFlip = 0;
  }

  // Swap/drop FX: done flights (and windows that vanished mid-flight) leave
  // the Model. WITHOUT this prune the record lingers forever, the animation
  // pump never disarms and the compositor recomposites the full monitor at
  // refresh rate indefinitely (the idle GPU-spike bug).
  for (auto it = m_swapfx.begin(); it != m_swapfx.end();)
    if (it->win.expired() || it->clock.raw(dropDur()) >= 1.0)
      it = m_swapfx.erase(it);
    else
      ++it;

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

double Overview::animMs(const char *leaf) const {
  if (cfg::anim.enabled == 0)
    return 1.0; // master off: clocks complete within one frame
  if (const auto *e = cfg::anim.leafEnabled(leaf); e && *e == 0)
    return 1.0; // leaf off
  const int ms = cfg::anim.leafMs(leaf)->get();
  if (ms >= 0)
    return std::max(1.0, static_cast<double>(ms));
  // _ms left at the sentinel (-1) → follow the legacy duration knob
  return std::max(1.0, static_cast<double>(cfg::anim.duration));
}

double Overview::newCardScale() const {
  if (!m_newCardAnim)
    return 1.0;
  const double p = m_newCard.raw(newCardDur());
  // pop curve from the registry ("back" default — a little overshoot)
  return curves::eval(anim("new_card").curve, p);
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
