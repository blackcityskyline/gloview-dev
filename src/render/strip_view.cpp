#include <algorithm>
#include <cmath>

#include <linux/input-event-codes.h>

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "../debug/log.hpp"
#include "gl_util.hpp"
#include "../config/config.hpp"
#include "../overview.hpp"
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include "window_content.hpp"

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

CBox box(const LRect &r) { return CBox{r.x, r.y, r.w, r.h}; }

// TEMP guard: renderRect RASSERTs on non-positive boxes ("width/height < 0"
// fires for 0 too) — log the offending box + site instead of aborting.
void safeRenderRect(const char *tag, const CBox &b, const CHyprColor &col,
                    Render::GL::CHyprOpenGLImpl::SRectRenderData opts) {
  if (b.width < 0 || b.height < 0) {
    debug::dbg(std::string("renderStrip NEG box @") + tag + ": " +
               std::to_string(b.width) + "x" + std::to_string(b.height) +
               " at " + std::to_string(b.x) + "," + std::to_string(b.y));
    return;
  }
  g_pHyprOpenGL->renderRect(b, col, opts);
}

} // namespace

// Z2: the workspace strip — translucent band, card frames/backings, "+"/"All"
// glyphs, per-slot backings and workspace labels. Card thumbnails are drawn
// right after this (renderStripWindows), hints/✕ after those
// (renderStripButtons) — everything that sits ON TOP of a thumbnail comes
// later in the painter, so no opaque window content can cover it.
void Overview::renderStrip() const {
  const auto m = m_monitor.lock();
  if (!m || m_strip.empty())
    return;
  const double e = eased();
  if (e <= 0.01)
    return;
  const double s =
      m->m_scale; // logical→pixel; Hyprland's renderRect wants pixel coords
  const int   previewRound = cfg::look.preview_round;
  const float roundPow     = cfg::look.preview_round_power;


  // Translucent band behind the cards (kept faint per request). The band
  // never uses native blur: it would sample currentFB, which can hold a
  // solitary fullscreen window (the workspace-switch fast path bypasses
  // shouldRenderWindow), leaking window content into the band. The backdrop
  // already provides the blur behind the strip — a flat band color is
  // sufficient.
  const auto bandCol = cfg::colors.strip_band.get(e);
  const LRect bandR  = stripBand();
  const Vector2D slide  = stripSlide(e);  // slide the whole strip in from its edge
  const Vector2D scroll = stripScroll();  // scroll the card group along the band
  safeRenderRect("band", pxb(CBox(bandR.x + slide.x, bandR.y + slide.y, bandR.w, bandR.h), s),
                 bandCol, {});

  const int  cardRound = cfg::strip.card_round;
  const auto cardBg    = cfg::colors.strip_card.get(e);
  const auto activeBg  = cfg::colors.strip_active.get(e);
  const auto activeLine = cfg::colors.strip_active_border.get(e);
  const auto hoverLine  = cfg::colors.strip_hover.get(e);
  const auto plusCol    = cfg::colors.strip_plus.get(e);
  const auto allCol     = cfg::colors.strip_all.get(e);
  // Expo indicator: when all-workspaces is active, the "All" card (if present)
  // lights up active-style; otherwise outline every real card for feedback.
  const bool allWs       = showAllWorkspaces();
  const bool allCardShown = cfg::strip.all_card != 0;


  for (size_t i = 0; i < m_strip.size(); ++i) {
    const auto &it = m_strip[i];
    const bool suppressContent =
        m_drag.lifted &&
        m_drag.press == model::Drag::Press::StripWin &&
        static_cast<int>(i) == m_drag.idx;
    const bool hover = static_cast<int>(i) == m_hoveredStrip;
    LRect card = it.card;
    card.x += slide.x + scroll.x; // follow the strip slide-in and scroll
    card.y += slide.y + scroll.y;
    if (m_newCardAnim && it.id == m_newCardId &&
        it.kind != model::StripItem::Kind::Plus && it.kind != model::StripItem::Kind::All) {
      const double f = newCardScale(); // pop-in: scale up from the card center
      const double cx = card.cx(), cy = card.cy();
      card = LRect{cx - card.w * f / 2.0, cy - card.h * f / 2.0, card.w * f,
                   card.h * f};
    }
    const CBox c = box(card);

    // border frame underlay: one rounded rect grown by the line width, so the
    // card body on top leaves a clean ring (four thin strips would blob at
    // the corners).
    const bool actLike =
        it.active || (allWs && allCardShown && it.kind == model::StripItem::Kind::All);
    const bool expoRing =
        allWs && !allCardShown && it.kind != model::StripItem::Kind::Plus;
    const bool ring = actLike || expoRing;
    if (ring || hover) {
      const auto &lc = ring ? activeLine : hoverLine;
      strokeRing(card, s, lc, actLike ? 3 : 2, cardRound, roundPow);
    }

    // The "All" card skips the active FILL: its 2x2 glyph shares the accent
    // color with the ring/fill in single-accent themes, and an accent-filled
    // body turned the whole card into one blob. Active reads via the thick
    // ring alone; the glyph sits on the dark body, clear of the ring.
    const bool allSkipFill = actLike && it.kind == model::StripItem::Kind::All;
    safeRenderRect("strip", pxb(c, s), (actLike && !allSkipFill) ? activeBg : cardBg,
             {.round = pxr(cardRound, s), .roundingPower = roundPow});

    if (it.kind == model::StripItem::Kind::Plus) {
      // centered plus glyph
      const double t = std::max(2.0, card.h * 0.04);
      const double L = std::min(card.w, card.h) * 0.34;
      const double cx = card.cx(), cy = card.cy();
      if (L > 0.5) { // a degenerate card has no glyph
        safeRenderRect("plusH", pxb(CBox(cx - L / 2, cy - t / 2, L, t), s), plusCol,
                 {.round = pxr(t / 2, s)});
        safeRenderRect("plusV", pxb(CBox(cx - t / 2, cy - L / 2, t, L), s), plusCol,
                 {.round = pxr(t / 2, s)});
      } else
        debug::dbg("plus glyph skipped: card " + std::to_string(card.w) + "x" +
                   std::to_string(card.h) + " at " + std::to_string(card.x) +
                   "," + std::to_string(card.y));
    } else if (it.kind == model::StripItem::Kind::All) {
      // 2x2 grid-of-squares glyph = "all windows / every workspace"
      const double pad = std::min(card.w, card.h) * 0.26;
      const double gw = card.w - 2 * pad, gh = card.h - 2 * pad;
      const double cg = std::max(2.0, std::min(card.w, card.h) * 0.07);
      const double cw = std::max(2.0, (gw - cg) / 2.0),
                   ch = std::max(2.0, (gh - cg) / 2.0);
      const double gx = card.x + pad, gy = card.y + pad;
      for (int r = 0; r < 2; ++r)
        for (int col = 0; col < 2; ++col)
          safeRenderRect("all", pxb(CBox(gx + col * (cw + cg), gy + r * (ch + cg), cw, ch), s),
                   allCol, {.round = pxr(2, s)});
    } else {
      // Empty workspace: a cover-fit wallpaper thumbnail fills the card —
      // the same source the backdrop uses (mpv/wallpaper texture, else the
      // frozen layers FBO), center-cropped via UVs and rounded like the
      // card. Without a backdrop source (blur off AND no direct texture)
      // the card stays flat.
      if (it.wins.empty() && cfg::strip.wallpaper != 0) {
        bool live = false;
        auto tex = backdropSource(live);
        if ((!tex || !tex->ok()) && m_backdropSrcFB &&
            m_backdropSrcFB->isAllocated())
          tex = m_backdropSrcFB->getTexture();
        if (tex && tex->ok()) {
          const double texW = std::max(1.0, static_cast<double>(tex->m_size.x));
          const double texH = std::max(1.0, static_cast<double>(tex->m_size.y));
          const double cardRatio = card.w / std::max(1.0, card.h);
          const double texRatio  = texW / texH;
          double u0 = 0.0, v0 = 0.0, u1 = 1.0, v1 = 1.0;
          if (cardRatio > texRatio) { // card wider than the image: crop top/bottom
            const double f = texRatio / cardRatio;
            v0 = (1.0 - f) / 2.0;
            v1 = (1.0 + f) / 2.0;
          } else {                    // card taller: crop left/right
            const double f = cardRatio / texRatio;
            u0 = (1.0 - f) / 2.0;
            u1 = (1.0 + f) / 2.0;
          }
          CTexPassElement::SRenderData td{};
          td.tex           = tex;
          td.box           = pxb(c, s);
          td.a             = static_cast<float>(e);
          td.round         = pxr(cardRound, s);
          td.roundingPower = roundPow;
          td.allowCustomUV = true;
          auto &uvTL = g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft;
          auto &uvBR = g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight;
          uvTL = Vector2D{u0, v0};
          uvBR = Vector2D{u1, v1};
          g_pHyprRenderer->draw(td, g_pHyprRenderer->m_renderData.damage);
          uvTL = Vector2D{-1.0, -1.0};
          uvBR = Vector2D{-1.0, -1.0};
        }
      }
      // Thin near-invisible backing per window slot: the thumbnail may carry
      // transparency, so without it the translucent card band over the
      // blurred backdrop bleeds through. NOT a decorative tint — see the
      // chrome-kernel comment in gl_util.hpp.
      if (!suppressContent) {
        for (size_t j = 0; j < it.wins.size(); ++j) {
          const auto &sw = it.wins[j];
          const auto w = sw.win.lock();
          if (!w || !w->m_isMapped || w->isHidden())
            continue;
          const LRect wbL = stripWinSlotRect(it, card, j);
          // TEMP: pin the degenerate-slot crash (safetyBacking -> renderRect
          // RASSERTs on negative dims; NaN slips past the max() clamps)
          if (!std::isfinite(wbL.w) || !std::isfinite(wbL.h) || wbL.w < 1 ||
              wbL.h < 1) {
            debug::dbg("renderStrip BAD slot: " + std::to_string(wbL.w) + "x" +
                       std::to_string(wbL.h) + " at " + std::to_string(wbL.x) +
                       "," + std::to_string(wbL.y) + " rel=" +
                       std::to_string(sw.rel.w) + "x" +
                       std::to_string(sw.rel.h) + " card=" +
                       std::to_string(card.w) + "x" + std::to_string(card.h));
            continue;
          }
          const int wRound = clampRound(previewRound, wbL.w, wbL.h);
          // Grab indicator: a bright highlight around the exact slot currently
          // pressed, before it's lifted into a floating drag — static (not a
          // pulse) so it doesn't force repainting while the mouse sits still.
          const bool grabbed = !m_drag.lifted &&
                               static_cast<int>(i) == m_drag.idx &&
                               static_cast<int>(j) == m_drag.winIdx;
          if (grabbed)
            strokeRing(wbL, s, cfg::colors.hover_border.get(e),
                       2, wRound, roundPow);
          safetyBacking(wbL, s, cfg::colors.backing.get(),
                        0.08 * e, wRound, roundPow);
        }
      }
    }

    // workspace label, centered above the card
    if (it.label && it.label->m_size.x > 0) {
      double lw = it.label->m_size.x;
      double lh = it.label->m_size.y;
      const double maxLw = card.w + 24.0;
      if (lw > maxLw) {
        const double k = maxLw / lw;
        lw *= k;
        lh *= k;
      }
      // labelH (26) is reserved above every card by buildStrip (both layouts).
      const double labelBand = 26.0;
      const double lx = card.cx() - lw / 2.0;
      const double ly = card.y - labelBand + (labelBand - lh) / 2.0;
      const float la =
          it.active ? static_cast<float>(e) : static_cast<float>(e) * 0.75F;
      g_pHyprOpenGL->renderTexture(it.label, pxb(CBox(lx, ly, lw, lh), s),
                                   {.a = la});
    }
  }
}

// Z2: the strip cards' live thumbnails — same slide-in + scroll offsets as
// renderStrip so they travel with their cards. The window being dragged is
// skipped here (it floats in Z3).
void Overview::renderStripWindows() const {
  const auto m = m_monitor.lock();
  if (!m || m_strip.empty())
    return;
  const double e = eased();
  if (e <= 0.01)
    return;
  const Vector2D slide  = stripSlide(e);
  const Vector2D scroll = stripScroll();
  const double scale = m->m_scale;
  const auto when    = Time::steadyNow();
  const int previewRound = cfg::look.preview_round;
  const float roundPow   = cfg::look.preview_round_power;

  for (size_t i = 0; i < m_strip.size(); ++i) {
    const auto &it = m_strip[i];
    if (it.kind == model::StripItem::Kind::Plus || it.kind == model::StripItem::Kind::All)
      continue;
    LRect card = it.card;
    card.x += slide.x + scroll.x;
    card.y += slide.y + scroll.y;
    for (size_t j = 0; j < it.wins.size(); ++j) {
      if (m_drag.lifted &&
          m_drag.press == model::Drag::Press::StripWin &&
          static_cast<int>(i) == m_drag.idx &&
          static_cast<int>(j) == m_drag.winIdx)
        continue; // being dragged as a floating preview right now
      const auto w = it.wins[j].win.lock();
      if (!w || !w->m_isMapped || w->isHidden())
        continue;
      if (swapfxActive(w))
        continue; // flying via a landing (Z2.5) — suppressed here
      // window slot inside the card, from its tiled goal position (logical)
      const LRect slot = stripWinSlotRect(it, card, j);
      const int round = pxr(clampRound(previewRound, slot.w, slot.h), scale);
      // renderWindowLive works in monitor PIXEL coords; the card chrome is
      // pre-scaled to pixels too (pxb), so surface and backing coincide at
      // any monitor scale.
      const CBox slotPx(slot.x * scale, slot.y * scale, slot.w * scale,
                        slot.h * scale);
      const CBox cardPx(card.x * scale, card.y * scale, card.w * scale,
                        card.h * scale);
      renderWindowLive(w, m, slotPx, cardPx, static_cast<float>(e), when,
                       round, roundPow);
    }
  }
}

// Z2: per-card "close every window on this workspace" ✕ + the drag
// destination hints — everything that sits ON TOP of the thumbnails, hence
// after renderStripWindows in the painter.
void Overview::renderStripButtons() const {
  const auto m = m_monitor.lock();
  if (!m || m_strip.empty())
    return;
  const double e = eased();
  if (e <= 0.01)
    return;
  const double s = m->m_scale;
  const int cardRound = cfg::strip.card_round;
  const float roundPow = cfg::look.preview_round_power;
  const bool showClose = m_desktopMode || closeButtonsAlwaysOn();
  const bool travelling = m_drag.travelSq() > 64.0; // ~8px
  const bool dropping = m_drag.armed() && m_drag.lifted && travelling;
  // True when this is an RMB drag (swap intent) from any source.
  const bool isRmb = dropping && m_drag.button == BTN_RIGHT;
  // Carrying a STRIP window: hovering an exact slot means swap intent —
  // real-slot rings for ANY button (both swap on slot drop); insert-zone
  // hints only apply away from slots.
  const bool rmbSwap = dropping && m_drag.press == model::Drag::Press::StripWin;

  // RMB swap-drag: ring the SOURCE slot once, wherever it lives.
  if (rmbSwap && m_drag.idx >= 0 &&
      m_drag.idx < static_cast<int>(m_strip.size()) &&
      m_drag.winIdx < static_cast<int>(m_strip[m_drag.idx].wins.size())) {
    strokeRingPx(pxb(stripWinSlotRect(m_strip[m_drag.idx],
                                      stripCardAt(m_drag.idx),
                                      m_drag.winIdx),
                     s),
                 cfg::colors.select_border.get(e),
                 0.7F * static_cast<float>(e),
                 clampRound(cfg::look.preview_round, 40, 40),
                 roundPow);
  }

  const auto dragW = m_drag.win.lock();

  for (size_t i = 0; i < m_strip.size(); ++i) {
    const auto &it = m_strip[i];
    if (it.kind == model::StripItem::Kind::Plus || it.kind == model::StripItem::Kind::All)
      continue;
    const LRect card = stripCardAt(i);

    // Drop-zone visualization:
    //   RMB (any source): highlight the exact slot under the cursor on
    //     EVERY card — the user is picking a swap partner, not a destination,
    //     so all windows are live targets regardless of which card they're on.
    //   LMB grid-tile or strip-win on hovered card: dwindle half-hint or
    //     full-ring depending on tile_layout.
    if (dropping) {
      const auto hoverCol = argb(cfg::colors.hover_border.get(e));
      const auto hintCol  = argb(cfg::colors.drop_hint.get(e));

      if (isRmb) {
        // Scan every slot on this card; highlight whichever one the cursor
        // is currently over. No hovered-card filter — RMB roams freely.
        for (size_t j = 0; j < it.wins.size(); ++j) {
          const auto v = it.wins[j].win.lock();
          if (!v || v == dragW || !v->m_isMapped || v->isHidden())
            continue;
          const LRect sl = stripWinSlotRect(it, card, j);
          if (!sl.contains(m_drag.x, m_drag.y))
            continue;
          const int sr = clampRound(cfg::look.preview_round, sl.w, sl.h);
          // Bright fill + ring: "this is your swap partner".
          safeRenderRect("rmbswaphint",
                         pxb(CBox{sl.x, sl.y, sl.w, sl.h}, s),
                         hintCol,
                         {.round = pxr(sr, s), .roundingPower = roundPow});
          strokeRingPx(pxb(sl, s), hoverCol,
                       0.9F * static_cast<float>(e), sr, roundPow);
          break;
        }
      } else if (static_cast<int>(i) == m_hoveredStrip) {
        // LMB on the hovered card: dwindle half-hint or legacy full ring.
        const bool dwindle = cfg::behavior.tile_layout.get() == "dwindle";
        bool slotFound = false;
        for (size_t j = 0; j < it.wins.size(); ++j) {
          const auto v = it.wins[j].win.lock();
          if (!v || v == dragW)
            continue;
          const LRect sl = stripWinSlotRect(it, card, j);
          if (!sl.contains(m_drag.x, m_drag.y))
            continue;
          slotFound = true;
          const int sr = clampRound(cfg::look.preview_round, sl.w, sl.h);
          if (dwindle) {
            LRect half = sl;
            if (sl.w >= sl.h) {
              half.w = sl.w / 2.0;
              if (m_drag.x >= sl.cx())
                half.x = sl.cx();
            } else {
              half.h = sl.h / 2.0;
              if (m_drag.y >= sl.cy())
                half.y = sl.cy();
            }
            const int hr = clampRound(cfg::look.preview_round, half.w, half.h);
            safeRenderRect("dwindlehalf",
                           pxb(CBox{half.x, half.y, half.w, half.h}, s),
                           hintCol,
                           {.round = pxr(hr, s), .roundingPower = roundPow});
            strokeRingPx(pxb(sl, s), hoverCol,
                         0.45F * static_cast<float>(e), sr, roundPow);
          } else {
            strokeRingPx(pxb(sl, s), hoverCol,
                         0.9F * static_cast<float>(e), sr, roundPow);
          }
          break;
        }
        if (!slotFound && it.wins.empty()) {
          safeRenderRect("emptycard", pxb(card, s), hintCol,
                         {.round = pxr(cardRound / 2, s), .roundingPower = roundPow});
        }
      }
    }

    // "close every window on this workspace" — the visible counterpart to
    // the middle-click shortcut. Stays a plain circle (default
    // roundingPower) regardless of preview_round_power — a "squircle" close
    // button would look broken at non-default curve exponents.
    if (showClose) {
      const LRect br = closeButtonRect(card);
      const double rad = br.w / 2.0;
      g_pHyprOpenGL->renderRect(
          pxb(box(br), s),
          cfg::colors.close_button.get(e),
          {.round = pxr(rad, s)});
      if (m_closeGlyph && m_closeGlyph->m_size.x > 0) {
        const double gw = m_closeGlyph->m_size.x * 0.62,
                     gh = m_closeGlyph->m_size.y * 0.62;
        g_pHyprOpenGL->renderTexture(
            m_closeGlyph,
            pxb(CBox(br.cx() - gw / 2.0, br.cy() - gh / 2.0, gw, gh), s),
            {.a = static_cast<float>(e)});
      }
    }
  }
}

} // namespace gloview
