#include <algorithm>
#include <chrono>
#include <cmath>

#include "../anim/curves.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"

namespace gloview {

namespace {

double lerp(double a, double b, double t) { return a + (b - a) * t; }

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

// Resolved animation group: {on, ms >= 1, curve}. One choke point for the
// config contract: master off -> instant; `<leaf>_enabled = 0` -> instant;
// `_ms = -1` inherits `duration`; an unknown name falls back to duration +
// easeout instead of dereferencing null.
anim::AnimCfg Overview::leaf(const char *name) const {
  anim::AnimCfg a;
  a.on = cfg::anim.enabled != 0;
  if (const auto *en = cfg::anim.leafEnabled(name); en && *en == 0)
    a.on = false;
  int ms = -1;
  if (const auto *m = cfg::anim.leafMs(name))
    ms = m->get();
  if (!a.on)
    a.ms = 1.0;
  else if (ms >= 0)
    a.ms = std::max(1.0, static_cast<double>(ms));
  else
    a.ms = std::max(1.0, static_cast<double>(cfg::anim.duration));
  if (const auto *c = cfg::anim.leafCurve(name))
    a.curve = c->get();
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
  return m_expoFlip > 0 ? "expo_in" : m_expoFlip < 0 ? "expo_out" : "glide";
}

double Overview::eased() const {
  // Chrome reveal/collapse follows its own leaf: open while entering, close
  // while exiting (m_progress is the LINEAR clock value either way).
  return curves::eval(leaf(m_opening ? "open" : "close").curve, m_progress);
}

double Overview::animDuration() const { return leaf(m_opening ? "open" : "close").ms; }

double Overview::tileProgress(int i) const {
  // Slot glides ride the tiles' forward clock (m_tileClock). CLOSE
  // deliberately goes back to riding m_progress DOWN: the tile lerp then has
  // the exact same shape as the collapsing chrome, keeping landing and
  // strip-collapse frame-synced.
  const double base =
      m_opening ? m_tileClock.raw(glideDur()) : m_progress;
  return staggered(base, i, static_cast<int>(m_tiles.size()));
}

double Overview::tileAppear(int i) const {
  const auto lf = leaf(entryLeaf());
  return curves::eval(
      lf.curve,
      staggered(m_rebuildClock.raw(lf.ms), i, static_cast<int>(m_tiles.size())));
}

LRect Overview::currentBox(const model::Tile &t, int i) const {
  const double e = curves::eval(leaf(glideLeaf()).curve, tileProgress(i));
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
  // top edge — mirrors of the ghost exits in renderGhosts.
  if (m_wsSlideDir != 0) {
    const std::string style = cfg::anim.ws_enter_anim.get();
    if (style == "slide" || style == "slidevert") {
      if (const auto m = m_monitor.lock()) {
        if (style == "slide")
          r.x += static_cast<double>(m_wsSlideDir) * m->m_size.x * (1.0 - ap);
        else
          r.y -= static_cast<double>(m->m_size.y) * (1.0 - ap);
      }
      return r;
    }
  }
  // Default entry: grow from the slot center.
  const double k  = 0.85 + 0.15 * ap;
  const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
  return LRect{cx - r.w * k / 2.0, cy - r.h * k / 2.0, r.w * k, r.h * k};
}

void Overview::updateAnimation() {
  const double dur = animDuration();
  // Stall guard first: a render hole rewinds active clocks to their last-known
  // position so wall-time inside the hole never counts.
  const auto nowTick = std::chrono::steady_clock::now();
  if (m_lastAnimTick.time_since_epoch().count() != 0) {
    const double gapMs =
        std::chrono::duration<double, std::milli>(nowTick - m_lastAnimTick)
            .count();
    if (gapMs > 100.0) {
      m_timeline.compensateStall(gapMs, dur);
      m_tileClock.compensateStall(gapMs, glideDur());
      if (m_newCardAnim)
        m_newCard.compensateStall(gapMs, newCardDur());
    }
  }
  m_lastAnimTick = nowTick;

  const auto stepMs = leaf("strip_step");
  if (!m_stripTween.done(stepMs.ms))
    m_stripScroll =
        lerp(m_stripScrollFrom, m_stripScrollTarget,
             curves::eval(stepMs.curve, m_stripTween.raw(stepMs.ms)));
  else
    m_stripScroll = m_stripScrollTarget;

  // Ghosts exit in their own leaf's window — clearing them in the entries'
  // window cut long exits mid-flight.
  const auto ghostsMs = leaf(ghostLeaf()).ms;
  if (!m_ghosts.empty() && m_rebuildClock.done(ghostsMs))
    m_ghosts.clear();

  // Swap pulses accumulate per animated frame with the delta CAPPED, so a
  // post-drop render hole cannot jump the ring through its overshoot plateau.
  const double pulseMs = leaf("pulse").ms;
  const auto nowTickP  = std::chrono::steady_clock::now();
  for (auto &p : m_pulses) {
    if (p.w.expired())
      continue;
    p.p += std::min(34.0, std::chrono::duration<double, std::milli>(
                              nowTickP - p.last).count()) /
           pulseMs;
    p.last = nowTickP;
  }
  std::erase_if(m_pulses, [](const model::WinPulse &p) {
    return p.w.expired() || p.p >= 1.0;
  });

  // Transition over only when BOTH sides finished (each on its own window);
  // compute the leaves before clearing the state they read.
  if (m_rebuildClock.done(ghostsMs) && m_rebuildClock.done(leaf(entryLeaf()).ms)) {
    m_wsSlideDir = 0;
    m_expoFlip = 0;
  }

  // Done flights (and vanished windows) leave the Model — without this prune
  // the pump never disarms and full-monitor recomposition runs forever.
  const auto dropMs = leaf("drop").ms;
  for (auto it = m_swapfx.begin(); it != m_swapfx.end();)
    it->win.expired() || it->clock.raw(dropMs) >= 1.0
        ? it = m_swapfx.erase(it)
        : ++it;

  const double t = m_timeline.raw(dur);
  m_progress = m_opening ? t : 1.0 - t;
  if (m_newCardAnim && m_newCard.done(newCardDur())) {
    m_newCardAnim = false;
    m_newCardId = 0;
  }
  // Close completion needs BOTH clocks done: chrome finishes early, but
  // flipping anything off before the tiles have landed would pop them
  // mid-air. Teardown runs from the painter tail after the final frame.
  if (!m_opening && t >= 1.0 && m_tileClock.done(dur)) {
    m_progress = 0.0;
    m_pendingDeactivate = true;
  }
}

double Overview::newCardScale() const {
  if (!m_newCardAnim)
    return 1.0;
  return curves::eval(leaf("new_card").curve, m_newCard.raw(newCardDur()));
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
