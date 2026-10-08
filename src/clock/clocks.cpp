#include <algorithm>
#include <chrono>

#include "../clock/curves.hpp"
#include "../anim.hpp"
#include "../config/config.hpp"
#include "../debug/log.hpp"
#include "../overview.hpp"

namespace gloview {

// Every clock here is pure linear progress; shaping lives in curves.cpp and
// durations are re-read from config on every call.
// The ladder spread is the user's, capped at 1: Stagger is the ceiling,
// StaggerPerTile grows it with the tile count and must not push past it.
double staggered(double base, int i, int n) {
  return anim::stagger(base, i, n,
                       std::min(anim::param(anim::Param::Stagger),
                                anim::param(anim::Param::StaggerPerTile) * n));
}

// imperative rule > schema key > default; off -> 1ms, speed divides base
anim::Id Overview::entryLeaf() const {
  if (m_view.expoFlip() != 0)
    return m_view.expoFlip() > 0 ? anim::Id::ExpoIn : anim::Id::ExpoOut;
  return m_view.wsSlideDir() != 0 ? anim::Id::WsEnter : anim::Id::Appear;
}

anim::Id Overview::ghostLeaf() const {
  if (m_view.expoFlip() != 0)
    return m_view.expoFlip() > 0 ? anim::Id::ExpoIn : anim::Id::ExpoOut;
  return m_view.wsSlideDir() != 0 ? anim::Id::WsExit : anim::Id::Appear;
}

anim::Id Overview::glideLeaf() const {
  if (m_view.jumpMode()) return m_opening ? anim::Id::JumpIn : anim::Id::JumpOut;
  if (m_view.expoFlip() != 0)
    return m_view.expoFlip() > 0 ? anim::Id::ExpoIn : anim::Id::ExpoOut;
  return carouselMode() ? anim::Id::CarouselStep : anim::Id::Glide;
}

// The master leaf is decided once, by masterLeaf(), and read by both the curve lookup
// below and animDuration() -- they were separate expressions of the same question and
// nothing checked that they agreed.
anim::Id Overview::masterId() const {
  switch (anim::masterLeaf(m_view.jumpMode(), m_opening)) {
    case anim::MasterLeaf::Open:    return anim::Id::Open;
    case anim::MasterLeaf::JumpOut: return anim::Id::JumpOut;
    case anim::MasterLeaf::Close:   break;
  }
  return anim::Id::Close;
}

// How far the close has run, 0 -> 1. m_progress runs DOWN on a close (1 open -> 0 closed);
// read forwards it looks exactly like no animation. Every exit fraction reads it here so
// the shrink, the fade and the clock cannot disagree about where the close is.
double Overview::exitT() const { return m_opening ? 0.0 : 1.0 - m_progress; }

double Overview::eased() const {
  return curves::eval(anim::cfg(masterId()).curve, m_progress);
}

// WINDOWED exit: hold the backdrop until our own tiles are done AND Hyprland has spent the
// forced full frames close() asked for -- those are the frames it rebuilds its blur in.
// Asking when the windows reached their goal geometry does NOT work: in the plain
// open/close the real windows never move (the plugin draws previews and hides them), so
// that test is true on frame 0 and holds nothing.
bool Overview::backdropSettled() const {
  const auto m = m_monitor.lock();
  return m_clocks.tiles.done(animDuration()) && (!m || m->m_forceFullFrames <= 0);
}

// The backdrop is part of the SAME gesture as the tiles, the strip and the chrome: it
// rides the open/close animation itself. That is the whole fix for the queued feel --
// separate clocks made the strip arrive first and then the backdrop set about arriving,
// with nothing joining them up. The gap a private clock was introduced for is handled by
// the BASE, not by a timeline: with windows present the base is the blurred cache, so the
// screen is blurred on frame 0 and the ramp has nothing to reveal anyway.
//
// WINDOWED exit is the one exception: the backdrop must stay until Hyprland has rebuilt
// its blur. Fading out before that is what exposed a sharp desktop mid-flight.
double Overview::blurAlpha() const {
  if (!anim::backdropFades(m_opening, m_landingEmpty))
    return 1.0;
  return std::clamp(eased(), 0.0, 1.0);
}

double Overview::animDuration() const { return anim::cfg(masterId()).ms; }

// CLOSE rides m_progress DOWN: tiles then match the collapsing chrome.
// Read m_clocks.tileMs, not m_glide.ms: a glideLeaf() flip jumps progress.
double Overview::tileProgress(int i) const {
  const double glideMs = (m_clocks.tileMs > 0) ? m_clocks.tileMs : m_glide.ms;
  // currentBox lerps natural -> target as e RISES, so close feeds it m_progress
  // as-is. Flipping it put every card on its natural box on the first close frame.
  const double base = m_opening ? m_clocks.tiles.raw(glideMs) : m_progress;
  // The FOCUSED tile is never staggered: stagger() assumes a rising 0..1, so on
  // close it would delay the landing — the same flight, timed differently.
  if (carouselMode() || i == m_scene.focused())
    return base;
  return staggered(base, i, static_cast<int>(m_scene.tiles().size()));
}

double Overview::tileAppear(int i) const {
  return curves::eval(
      m_entry.curve,
      staggered(m_clocks.rebuild.raw(m_entry.ms), i,
                static_cast<int>(m_scene.tiles().size())));
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
  // The deck's hide motion, in the grid as well as the run. A card that cannot land was
  // pinned to its own box by close(); without this it simply stood there while the
  // backdrop faded and was gone in a frame. The run stands down here: it knows the card's
  // depth, which this does not, and applies the same predicate itself -- two shrinks of the
  // same card would compound into k*k.
  if (!carouselMode() && !m_opening && anim::cardHides(0, tileGoesHome(t))) {
    const double k =
        anim::hideShrink(exitT(), anim::param(anim::Param::Collapse));
    const double cx = r.cx(), cy = r.cy();
    r = LRect{cx - r.w * k * 0.5, cy - r.h * k * 0.5, r.w * k, r.h * k};
  }
  const double ap = t.appear < 1.0 ? tileAppear(i) : 1.0;
  if (ap >= 1.0)
    return r;
  if (m_view.wsSlideDir() != 0 && m_enterStyle != "pop") {
    if (const auto m = m_monitor.lock()) {
      if (m_enterStyle == "slide")
        r.x += static_cast<double>(m_view.wsSlideDir()) * m->m_size.x * (1.0 - ap);
      else if (m_enterStyle == "slidevert")
        r.y -= static_cast<double>(m->m_size.y) * (1.0 - ap);
    }
    if (m_enterStyle == "slide" || m_enterStyle == "slidevert" ||
        m_enterStyle == "fade")
      return r;
  }
  // How small a tile is at the start of its scale-in, from anim::param so a user can
  // deepen or flatten the pop. The 0.15 was a literal here and meant to be tuned.
  const double k = 1.0 - anim::param(anim::Param::PopScale) * (1.0 - ap);
  const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
  return LRect{cx - r.w * k / 2.0, cy - r.h * k / 2.0, r.w * k, r.h * k};
}

void Overview::updateAnimation() {
  m_glide      = anim::cfg(glideLeaf());
  m_entry      = anim::cfg(entryLeaf());
  m_ghost      = anim::cfg(ghostLeaf());
  m_lift       = anim::cfg(anim::Id::Lift);
  m_enterStyle = anim::style("ws_enter");
  m_exitStyle  = anim::style("ws_exit");

  const double dur = animDuration();
  // stall guard: rewind every clock past a render hole
  const auto now = std::chrono::steady_clock::now();
  if (m_lastAnimTick.time_since_epoch().count() != 0) {
    const double gapMs =
        std::chrono::duration<double, std::milli>(now - m_lastAnimTick).count();
    if (gapMs > 100.0) {
      const double glideMs = glideDur();
      m_clocks.master.compensateStall(gapMs, dur);
      m_clocks.tiles.compensateStall(gapMs, glideMs);
      m_clocks.rebuild.compensateStall(gapMs, std::max(m_entry.ms, m_ghost.ms));
      m_clocks.strip.compensateStall(gapMs, anim::cfg(anim::Id::StripStep).ms);
      if (m_newCardAnim)
        m_clocks.newCard.compensateStall(gapMs, newCardDur());
      if (m_drag.lifted)
        m_clocks.dragLift.compensateStall(gapMs, anim::cfg(anim::Id::Lift).ms);
      m_fx.eachSwap([&](auto &fx) { fx.clock.compensateStall(gapMs, fx.ms); });
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

  const auto st = anim::cfg(anim::Id::StripStep);
  if (!m_clocks.strip.done(st.ms))
    m_stripScroll =
        lerp(m_stripScrollFrom, m_stripScrollTarget,
             curves::eval(st.curve, m_clocks.strip.raw(st.ms)));
  else
    m_stripScroll = m_stripScrollTarget;

  if (!m_fx.ghosts().empty() && m_clocks.rebuild.done(m_ghost.ms))
    m_fx.clearGhosts();
  // entries must settle explicitly or they keep evaluating curves forever
  if (m_clocks.rebuild.done(m_entry.ms) &&
      std::any_of(m_scene.tiles().begin(), m_scene.tiles().end(),
                  [](const model::Tile &t) { return t.appear < 1.0; }))
    for (auto &t : m_scene.edit())
      t.appear = 1.0;

  const auto pulse = anim::cfg(anim::Id::Pulse);
  m_fx.eachPulse([&](auto &p) {
    if (p.w.expired())
      return;
    p.p += std::min(34.0, std::chrono::duration<double, std::milli>(now - p.last)
                              .count()) /
           pulse.ms;
    p.last = now;
  });
  // Pruned on each pulse's own accumulated fraction: a live config change must not
  // shorten a ring that is already running.
  m_fx.dropPulse(
      [](const model::WinPulse &p) { return p.w.expired() || p.p >= 1.0; });

  if (m_clocks.rebuild.done(m_ghost.ms) && m_clocks.rebuild.done(m_entry.ms)) {
    m_view.setWsSlideDir( 0);
    m_view.setExpoFlip( 0);
    if (m_pendingJumpClose) {
      m_pendingJumpClose = false;
      close();
    }
  }

  // prune on each flight's own clock; live config would cut landings short
  m_fx.dropSwap([](const model::SwapFX &fx) {
    return fx.win.expired() || fx.clock.raw(fx.ms) >= 1.0;
  });

  const double t = m_clocks.master.raw(dur);
  m_progress = m_opening ? t : 1.0 - t;
  if (m_newCardAnim && m_clocks.newCard.done(newCardDur())) {
    m_newCardAnim = false;
    m_newCardId = 0;
  }
  // NEVER zero m_progress: a transparent frame before deactivate() flashes.
  // Clamp as soon as the flight ends, NOT together with the deactivate: the exit ramp
  // outlives the close animation, and at exactly 0 the overlay paints nothing, Hyprland
  // has no damage to serve, and the ramp runs on with no frames to show it (measured:
  // 769ms of ticks with no render).
  if (!m_opening && t >= 1.0 && m_progress <= 0.0)
    m_progress = 1e-6;
  // The overlay outlives the close animation by exactly the windowed-exit hold: that is
  // what keeps the backdrop covering the screen until Hyprland's own blur has taken over
  // on the windows that have just landed. Lingering past that would leave a transparent
  // overlay over a sharp desktop, which is the gap all over again.
  if (!m_opening && t >= 1.0 && m_clocks.tiles.done(dur) && backdropSettled())
    m_pendingDeactivate = true;
}

bool Overview::animBusy() const {
  return m_active &&
         (secondaryAnimsActive() || !m_clocks.tiles.done(glideDur()) ||
          m_newCardAnim || m_drag.lifted || !m_fx.swapfx().empty() ||
          m_pendingJumpClose || m_pendingDeactivate ||
          (m_opening && m_progress < 1.0) || (!m_opening && m_progress > 0.0));
}

double Overview::newCardScale() const {
  if (!m_newCardAnim)
    return 1.0;
  return curves::eval(anim::cfg(anim::Id::Card).curve, m_clocks.newCard.raw(newCardDur()));
}

void Overview::animateStripTo(double from, double to) {
  from = std::clamp(from, 0.0, std::max(0.0, m_stripScrollMax));
  to   = std::clamp(to, 0.0, std::max(0.0, m_stripScrollMax));
  const auto st = anim::cfg(anim::Id::StripStep);
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
