#include <algorithm>
#include <chrono>

#include "../clock/curves.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"

namespace gloview {

// Every clock here is pure linear progress; shaping lives in curves.cpp and
// durations are re-read from config on every call.
namespace {

double staggered(double base, int i, int n) {
  if (n <= 1)
    return base;
  const double spread = std::min(0.08, 0.015 * n);
  const double start  = spread * (static_cast<double>(i) / (n - 1));
  return std::clamp((base - start) / std::max(0.001, 1.0 - spread), 0.0, 1.0);
}

}

// imperative rule > schema key > default; off -> 1ms, speed divides base
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
  const char *leaf_name = (m_jumpMode && !m_opening) ? "jump_out"
                        : m_opening ? "open" : "close";
  return curves::eval(leaf(leaf_name).curve, m_progress);
}

double Overview::animDuration() const {
  if (m_jumpMode && !m_opening) return leaf("jump_out").ms;
  return leaf(m_opening ? "open" : "close").ms;
}

// CLOSE rides m_progress DOWN: tiles then match the collapsing chrome.
// Read m_clocks.tileMs, not m_glide.ms: a glideLeaf() flip jumps progress.
double Overview::tileProgress(int i) const {
  const double glideMs = (m_clocks.tileMs > 0) ? m_clocks.tileMs : m_glide.ms;
  // currentBox lerps natural -> target as e RISES, so close feeds it m_progress
  // as-is. Flipping it put every card on its natural box on the first close frame.
  const double base = m_opening ? m_clocks.tiles.raw(glideMs) : m_progress;
  // The SELECTED tile is never staggered: stagger() assumes a rising 0..1, so on
  // close it would delay the landing — the same flight, timed differently.
  if (carouselMode() || i == m_selected)
    return base;
  return staggered(base, i, static_cast<int>(m_tiles.size()));
}

double Overview::tileAppear(int i) const {
  return curves::eval(
      m_entry.curve,
      staggered(m_clocks.rebuild.raw(m_entry.ms), i,
                static_cast<int>(m_tiles.size())));
}

LRect Overview::currentBox(const model::Tile &t, int i) const {
  const std::string &curve = m_clocks.tileCurve.empty() ? m_glide.curve : m_clocks.tileCurve;
  const double e = curves::eval(curve, tileProgress(i));
  const auto &a = t.natural;
  const auto &b = t.target;
  LRect r{lerp(a.x, b.x, e), lerp(a.y, b.y, e), lerp(a.w, b.w, e),
          lerp(a.h, b.h, e)};
  // A card off the carousel run has a zeroed target. Nothing paints it, so it
  // must not resize on its way there either — hold the box it was last shown at.
  if (carouselMode() && b.w < 1.0 && a.w >= 1.0)
    r = a;
  const double ap = t.appear < 1.0 ? tileAppear(i) : 1.0;
  if (ap >= 1.0)
    return r;
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
  m_glide      = leaf(glideLeaf());
  m_entry      = leaf(entryLeaf());
  m_ghost      = leaf(ghostLeaf());
  m_lift       = leaf("drag");
  m_enterStyle = cfg::animStyle("ws_enter");
  m_exitStyle  = cfg::animStyle("ws_exit");

  const double dur = animDuration();
  // stall guard: rewind every clock past a render hole
  const auto now = std::chrono::steady_clock::now();
  if (m_lastAnimTick.time_since_epoch().count() != 0) {
    const double gapMs =
        std::chrono::duration<double, std::milli>(now - m_lastAnimTick).count();
    if (gapMs > 100.0) {
      const double glideMs = (m_clocks.tileMs > 0) ? m_clocks.tileMs : m_glide.ms;
      m_clocks.master.compensateStall(gapMs, dur);
      m_clocks.tiles.compensateStall(gapMs, glideMs);
      m_clocks.rebuild.compensateStall(gapMs, std::max(m_entry.ms, m_ghost.ms));
      m_clocks.strip.compensateStall(gapMs, leaf("strip_step").ms);
      if (m_newCardAnim)
        m_clocks.newCard.compensateStall(gapMs, newCardDur());
      if (m_drag.lifted)
        m_clocks.dragLift.compensateStall(gapMs, leaf("drag").ms);
      for (auto &fx : m_swapfx)
        fx.clock.compensateStall(gapMs, fx.ms);
    }
  }
  m_lastAnimTick = now;

  if (m_drag.armed() && !m_drag.lifted) {
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        now - m_drag.holdStartMs).count();
    if (elapsedMs >= static_cast<double>(cfg::behavior.hold_lift_ms.get())) {
      m_drag.lifted = true;
      m_clocks.dragLift.begin();
      damage();
    } else {
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
  if (!m_clocks.strip.done(st.ms))
    m_stripScroll =
        lerp(m_stripScrollFrom, m_stripScrollTarget,
             curves::eval(st.curve, m_clocks.strip.raw(st.ms)));
  else
    m_stripScroll = m_stripScrollTarget;

  if (!m_ghosts.empty() && m_clocks.rebuild.done(m_ghost.ms))
    m_ghosts.clear();
  // entries must settle explicitly or they keep evaluating curves forever
  if (m_clocks.rebuild.done(m_entry.ms) &&
      std::any_of(m_tiles.begin(), m_tiles.end(),
                  [](const model::Tile &t) { return t.appear < 1.0; }))
    for (auto &t : m_tiles)
      t.appear = 1.0;

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

  if (m_clocks.rebuild.done(m_ghost.ms) && m_clocks.rebuild.done(m_entry.ms)) {
    m_wsSlideDir = 0;
    m_expoFlip = 0;
    if (m_pendingJumpClose) {
      m_pendingJumpClose = false;
      close();
    }
  }

  if (m_clocks.tileMs > 0 && m_clocks.tiles.done(m_clocks.tileMs)) {
    m_clocks.tileMs = 0;
    m_clocks.tileCurve.clear();
  }

  // prune on each flight's own clock; live config would cut landings short
  for (auto it = m_swapfx.begin(); it != m_swapfx.end();) {
    if (it->win.expired() || it->clock.raw(it->ms) >= 1.0)
      it = m_swapfx.erase(it);
    else
      ++it;
  }

  const double t = m_clocks.master.raw(dur);
  m_progress = m_opening ? t : 1.0 - t;
  if (m_newCardAnim && m_clocks.newCard.done(newCardDur())) {
    m_newCardAnim = false;
    m_newCardId = 0;
  }
  // NEVER zero m_progress: a transparent frame before deactivate() flashes
  if (!m_opening && t >= 1.0 && m_clocks.tiles.done(dur)) {
    m_progress = 1e-6;
    m_pendingDeactivate = true;
  }
}

bool Overview::animBusy() const {
  return m_active &&
         (secondaryAnimsActive() || !m_clocks.tiles.done(glideDur()) ||
          m_newCardAnim || m_drag.lifted || !m_swapfx.empty() ||
          m_pendingJumpClose || m_pendingDeactivate ||
          (m_opening && m_progress < 1.0) || (!m_opening && m_progress > 0.0));
}

double Overview::newCardScale() const {
  if (!m_newCardAnim)
    return 1.0;
  return curves::eval(leaf("card").curve, m_clocks.newCard.raw(newCardDur()));
}

void Overview::animateStripTo(double from, double to) {
  from = std::clamp(from, 0.0, std::max(0.0, m_stripScrollMax));
  to   = std::clamp(to, 0.0, std::max(0.0, m_stripScrollMax));
  const auto st = leaf("strip_step");
  if (!st.on || std::abs(to - from) < 0.5) {
    m_stripScrollFrom = m_stripScrollTarget = m_stripScroll = to;
    return;
  }
  m_stripScrollFrom = !m_clocks.strip.done(st.ms) ? m_stripScroll : from;
  m_stripScrollTarget = to;
  m_clocks.strip.begin();
  ensureAnimPump();
}

}
