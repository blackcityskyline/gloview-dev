#include "../config/config.hpp"
#include "../debug/log.hpp"
#include "../overview.hpp"
#include "../render/gl_util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <set>
#include <utility>

#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

void Overview::buildTiles() {
  // Canvas-parked boxes must survive this rebuild: carry each old tile's
  // target/parked onto its window's fresh Tile.
  std::unordered_map<PHLWINDOWREF, std::pair<LRect, bool>> priorTargets;
  for (const auto &t : m_tiles)
    if (t.parked)
      if (const auto w = t.win.lock())
        priorTargets.emplace(w, std::pair{t.target, true});
  m_tiles.clear();
  const auto m = m_monitor.lock();
  const auto ws = m_workspace.lock();
  if (!m || !ws)
    return;

  // Off-workspace windows (expo) render live from their last-committed texture,
  // same as the strip cards. Membership shared with syncTiles via
  // tileBelongs().
  for (const auto &w : Desktop::windowState()->windows()) {
    if (!tileBelongs(w, m, ws))
      continue;

    model::Tile t;
    t.win = w;
    if (const auto it = priorTargets.find(w); it != priorTargets.end()) {
      t.target = it->second.first;
      t.parked = true;
    }
    // settled goal(), not value(): a mid-desktop-jump value() carries the
    // workspace-slide offset and would warp every preview.
    const auto p = w->positionAnimation()->goal();
    const auto s = w->sizeAnimation()->goal();
    t.natural = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                      std::max(1.0, s.x), std::max(1.0, s.y)};
    m_tiles.push_back(t);
  }

  // cache each window's title texture, drawn under the tile on hover.
  if (g_pHyprOpenGL && g_pHyprRenderer) {
    g_pHyprOpenGL->makeEGLCurrent();
    const auto lblCol = cfg::colors.label.get();
    for (auto &t : m_tiles) {
      const auto w = t.win.lock();
      if (!w)
        continue;
      std::string text = w->m_title;
      if (text.empty())
        text = w->m_class;
      if (text.size() > 80)
        text = text.substr(0, 79) + "…";
      t.label = cachedLabel(w.get(), text, lblCol, 15);
    }
    // pre-render the "✕" (or configured) glyph once per icon (rendering text
    // mid-pass is unsafe); re-rendered whenever close_button_icon changes so it
    // takes effect live.
    const std::string closeIcon =
        cfg::look.close_button_icon.get();
    if (!m_closeGlyph || m_closeGlyphIcon != closeIcon) {
      m_closeGlyph = g_pHyprRenderer->renderText(closeIcon,
          cfg::colors.close_glyph.get(), 16,
          false, "", 0, 800);
      m_closeGlyphIcon = closeIcon;
    }
  }
}

void Overview::buildStrip() {
  m_strip.clear();
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const auto cur = m_workspace.lock();

  // Three explicit modes instead of a plain on/off: "show" (default) shows
  // every numeric workspace up to the highest one in use, even ones that have
  // NEVER been created — "neighbors" hides empty ones EXCEPT the displayed
  // workspace's immediate numeric neighbors, cascading one hop at a time as you
  // navigate into a run of them — "hide" is a pure filter: only occupied
  // workspaces (plus whichever one is currently displayed), no empty exceptions
  // at all. Hyprland only keeps a PHLWORKSPACE object for a workspace that was
  // actually created/visited — an empty, never-visited one simply has no object
  // to iterate — so "show"/"neighbors" can't just filter getWorkspaces(); the
  // IDs with no real object get a VIRTUAL placeholder card
  // (StripItem::virtualWs) instead, which lazily creates the real workspace the
  // same way "+" does, but at that specific number.
  const std::string emptyMode =
      cfg::strip.empty_mode.get();
  const bool showSpecial = cfg::behavior.show_special != 0;
  const auto wsHasWindows = [](const PHLWORKSPACE &w) {
    for (const auto &win : Desktop::windowState()->windows())
      if (win && win->m_isMapped && !win->isHidden() && win->m_workspace == w)
        return true;
    return false;
  };

  std::unordered_map<int, PHLWORKSPACE>
      real; // numeric id -> existing workspace object, this monitor
  const int curId = (cur && !cur->m_isSpecialWorkspace) ? cur->m_id : -1;
  int highestId = 0;
  for (const auto &wref : State::workspaceState()->workspaces()) {
    const auto ws = wref.lock();
    if (!ws || ws->m_monitor.lock() != m)
      continue;
    if (ws->m_isSpecialWorkspace) {
      if (showSpecial &&
          wsHasWindows(ws)) { // a scratchpad is only meaningful when populated
        model::StripItem it;
        it.ws = ws;
        it.id = ws->m_id;
        it.active = (ws == cur);
        m_strip.push_back(std::move(it));
      }
      continue;
    }
    if (ws->m_id <= 0)
      continue;
    real[ws->m_id] = ws;
    highestId = std::max(highestId, static_cast<int>(ws->m_id));
  }
  // special-workspace cards (if any) were pushed straight into m_strip above
  // and don't participate in numeric id ordering below; stash them aside and
  // re-append after.
  std::vector<model::StripItem> specialCards;
  specialCards.swap(m_strip);

  std::set<int> idsToShow;
  if (emptyMode == "hide") {
    for (const auto &[id, ws] : real)
      if (ws == cur || wsHasWindows(ws))
        idsToShow.insert(id);
  } else if (emptyMode == "neighbors") {
    for (const auto &[id, ws] : real)
      if (ws == cur || wsHasWindows(ws))
        idsToShow.insert(id);
    // cascading reveal: the displayed workspace's immediate numeric neighbors,
    // even when neither has a real object yet — re-evaluated every buildStrip()
    // call (i.e. every time the displayed workspace changes), so switching onto
    // a freshly-revealed neighbor reveals ITS neighbor next, one hop at a time.
    if (curId > 0) {
      if (curId - 1 > 0)
        idsToShow.insert(curId - 1);
      idsToShow.insert(curId + 1);
    }
  } else { // "show": every numeric id up to the highest one in use, even if
           // never created.
    // At least the 10 quick-access slots (key_workspace's default 1..0 =
    // workspaces 1..10) are always offered; extend further if a higher
    // workspace already exists.
    const int upTo = std::max({10, highestId, curId});
    for (int id = 1; id <= upTo; ++id)
      idsToShow.insert(id);
    for (const auto &[id, ws] :
         real) // a manually-created id beyond `upTo` still shows
      idsToShow.insert(id);
  }
  if (curId > 0)
    idsToShow.insert(
        curId); // whatever's displayed always stays, occupied or not

  // optional leading "All workspaces" card (toggles expo). Pushed FIRST so it
  // sits at the strip's leading edge; skipped wherever cards are treated as
  // workspaces (see the Kind::All guards).
  if (cfg::strip.all_card != 0) {
    model::StripItem all;
    all.kind = model::StripItem::Kind::All;
    all.id = 0;
    m_strip.push_back(std::move(all));
  }

  for (const int id : idsToShow) {
    model::StripItem it;
    it.id = id;
    const auto found = real.find(id);
    if (found == real.end()) {
      it.virtualWs = true; // no real PHLWORKSPACE object yet — lazily created
                           // on click/drop
      m_strip.push_back(std::move(it));
      continue;
    }
    const auto &ws = found->second;
    it.ws = ws;
    it.active = (ws == cur);
    for (const auto &w : Desktop::windowState()->windows()) {
      if (!w || !w->m_isMapped || w->isHidden() || w->m_workspace != ws)
        continue;
      const auto p = w->positionAnimation()->goal();
      const auto s = w->sizeAnimation()->goal();
      model::StripWin sw;
      sw.win = w;
      sw.rel = LRect{(p.x - m->m_position.x) / m->m_size.x,
                     (p.y - m->m_position.y) / m->m_size.y, s.x / m->m_size.x,
                     s.y / m->m_size.y};
      it.wins.push_back(sw);
    }
    m_strip.push_back(std::move(it));
  }
  for (auto &sc :
       specialCards) // scratchpad card(s), if shown, sit after the numeric ones
    m_strip.push_back(std::move(sc));

  // trailing "+" card to create a new workspace
  model::StripItem plus;
  plus.kind = model::StripItem::Kind::Plus;
  plus.id = 0;
  m_strip.push_back(std::move(plus));

  // render workspace name labels (cached textures) up front
  if (g_pHyprOpenGL && g_pHyprRenderer) {
    g_pHyprOpenGL->makeEGLCurrent();
    const auto lblCol = cfg::colors.label.get();
    for (auto &it : m_strip) {
      if (it.kind == model::StripItem::Kind::Plus)
        continue;
      if (it.kind == model::StripItem::Kind::All) {
        it.label = cachedLabel((void *)-2, "All workspaces", lblCol, 13);
        continue;
      }
      const auto ws = it.ws.lock();
      std::string nm = ws ? ws->m_name : std::to_string(it.id);
      const bool numeric =
          !nm.empty() && std::all_of(nm.begin(), nm.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c));
          });
      const std::string text = numeric ? ("Workspace " + nm) : nm;
      it.label =
          cachedLabel(ws ? (void *)ws.get() : (void *)(intptr_t)it.id, text,
                      lblCol, 13);
    }
  }

  // --- lay out the cards: monitor-aspect cards along the band, trailing "+" as
  // the last.
  //     Top/Bottom → horizontal row, Left/Right → vertical column. Each
  //     reserves labelH above it for the name. The whole sequence (cards + "+")
  //     is one scrollable group: centered when it fits, else scrolled
  //     (m_stripScroll) so off-screen ends clip at the monitor edges and
  //     nothing spills into the preview area.
  const LRect band = stripBand();
  const bool horiz = stripHorizontal();
  const double margin = cfg::strip.margin;
  const double gap = cfg::strip.gap;
  const double labelH = 26.0;
  const double aspect = m->m_size.x / std::max(1.0, m->m_size.y);

  double cardW, cardH;
  if (horiz) {
    cardH = std::max(10.0, band.h - 2 * margin - labelH);
    cardW = cardH * aspect;
  } else {
    cardW = std::max(10.0, band.w - 2 * margin);
    cardH = cardW / aspect;
  }

  // size one card occupies along the band's main axis (a vertical column adds
  // the label height per card; a horizontal row's labels live in the band's top
  // margin)
  const double cellMain = horiz ? cardW : (cardH + labelH);
  const int n = static_cast<int>(m_strip.size()); // includes the "+"
  const double bandMain = horiz ? band.w : band.h;
  const double availMain = std::max(1.0, bandMain - 2 * margin);
  const double groupMain = n * cellMain + std::max(0, n - 1) * gap;
  const double mainOrigin = (horiz ? band.x : band.y) + margin;

  // base position of the first card (scroll == 0). Centered when the group
  // fits, else flush to the start and scrollable.
  m_stripScrollMax = std::max(0.0, groupMain - availMain);
  const double start = (m_stripScrollMax <= 0.0)
                           ? mainOrigin + (availMain - groupMain) / 2.0
                           : mainOrigin;

  const double cardX = band.x + margin;          // vertical: card column x
  const double cardY = band.y + margin + labelH; // horizontal: card row y

  double main = start;
  for (auto &it : m_strip) {
    if (horiz)
      it.card = LRect{main, cardY, cardW, cardH};
    else
      it.card = LRect{cardX, main + labelH, cardW, cardH};
    main += cellMain + gap;
  }

  // On open / rebuild, scroll the active workspace into view (centered) when
  // the group overflows. If a strip was already on screen, GLIDE to the new
  // offset (strip_step) — that is what makes each workspace step read as one
  // motion; a first build snaps (from==want on a fresh session).
  const double prevScroll = m_stripScrollTarget;
  m_stripScroll = 0.0;
  double want = 0.0;
  if (m_stripScrollMax > 0.0) {
    for (const auto &it : m_strip) {
      if (!it.active)
        continue;
      const double cardCenter =
          (horiz ? it.card.x + it.card.w / 2.0 : it.card.y + it.card.h / 2.0);
      const double viewCenter = mainOrigin + availMain / 2.0;
      want = std::clamp(cardCenter - viewCenter, 0.0, m_stripScrollMax);
      break;
    }
  }
  animateStripTo(prevScroll, want);
}

// Scroll offset of the strip group along its main axis (horizontal → x,
// vertical → y), applied on top of each card's base position wherever cards are
// drawn or hit-tested.
Vector2D Overview::stripScroll() const {
  return stripHorizontal() ? Vector2D{-m_stripScroll, 0.0}
                           : Vector2D{0.0, -m_stripScroll};
}

LRect Overview::stripCardAt(size_t i) const {
  if (i >= m_strip.size())
    return LRect{0, 0, 0, 0};
  const LRect &c = m_strip[i].card;
  const Vector2D scroll = stripScroll();
  const Vector2D slide =
      stripSlide(eased()); // strip cards slide in/out with the open/close
  // animation (renderStrip/renderStripWindows apply the same offset) — omitting
  // it here made anything hit-tested or drawn via this helper (notably the
  // strip close buttons) sit glued at the settled position while the card body
  // visibly slid past underneath (task #7).
  return LRect{c.x + slide.x + scroll.x, c.y + slide.y + scroll.y, c.w, c.h};
}

// A strip window's on-screen slot rect given the CARD rect it sits in (already
// scroll/slide-adjusted by the caller — shared by rendering and hit-testing so
// both always agree on where a given window's preview actually is).
LRect Overview::stripWinSlotRect(const model::StripItem &it, const LRect &card,
                                 size_t j) const {
  if (j >= it.wins.size())
    return LRect{0, 0, 0, 0};
  const auto &rel = it.wins[j].rel;
  return LRect{card.x + rel.x * card.w, card.y + rel.y * card.h,
               std::max(2.0, rel.w * card.w), std::max(2.0, rel.h * card.h)};
}

void Overview::layoutTiles() {
  const auto m = m_monitor.lock();
  if (!m || m_tiles.empty())
    return;

  LayoutCfg cfg;
  cfg.engine = parseEngine(cfg::grid.layout.get().c_str());
  cfg.gap = cfg::grid.gap;
  const int padX = cfg::grid.padding;
  const int padT = cfg::grid.padding_top;
  const int padB = cfg::grid.padding_bottom;
  cfg.padLeft = padX;
  cfg.padRight = padX;
  cfg.padTop = padT;
  cfg.padBottom = padB;
  // Keep the main previews clear of the strip: reserve the band's full
  // footprint (offset + thickness) on whichever edge it is anchored to, on top
  // of the configured breathing room.
  const double bandSpan = stripThickness() + stripOffset();
  switch (stripAnchor()) {
  case Anchor::Bottom:
    cfg.padBottom += bandSpan;
    break;
  case Anchor::Left:
    cfg.padLeft += bandSpan;
    break;
  case Anchor::Right:
    cfg.padRight += bandSpan;
    break;
  case Anchor::Top:
  default:
    cfg.padTop += bandSpan;
    break;
  }
  cfg.maxScale = cfg::grid.max_scale;

  // Desktop (canvas) mode: fit the WHOLE monitor into the usable area and place
  // each preview at its real scaled position — a shrunk live desktop. A dragged
  // preview keeps its parked spot (t.parked), sticky across rebuilds. Never
  // touches real windows.
  if (m_desktopMode) {
    const double usableX = cfg.padLeft;
    const double usableY = cfg.padTop;
    const double usableW =
        std::max(1.0, m->m_size.x - cfg.padLeft - cfg.padRight);
    const double usableH =
        std::max(1.0, m->m_size.y - cfg.padTop - cfg.padBottom);
    const double s = std::min({usableW / m->m_size.x, usableH / m->m_size.y,
                               static_cast<double>(cfg.maxScale)});
    m_desktopS = s;
    m_desktopOx = usableX + (usableW - m->m_size.x * s) / 2.0;
    m_desktopOy = usableY + (usableH - m->m_size.y * s) / 2.0;
    for (auto &t : m_tiles) {
      if (t.parked) // user-placed: nothing to compute
        continue;
      t.target = LRect{m_desktopOx + t.natural.x * s,
                       m_desktopOy + t.natural.y * s,
                       std::max(1.0, t.natural.w * s),
                       std::max(1.0, t.natural.h * s)};
    }
    return;
  }

  std::vector<LRect> nat;
  nat.reserve(m_tiles.size());
  for (const auto &t : m_tiles)
    nat.push_back(t.natural);

  const auto out =
      computeLayout(nat, LRect{0, 0, m->m_size.x, m->m_size.y}, cfg);
  for (size_t i = 0; i < m_tiles.size(); ++i)
    m_tiles[i].target = out[i];
}

// Rebuild tiles/strip, then glide each survivor from its captured box into its
// new slot without re-running the chrome reveal (m_progress pinned at 1).
// Shared by drop-to-workspace and close-window.
std::vector<std::pair<PHLWINDOW, LRect>>
Overview::captureCurrentBoxes(PHLWINDOW exclude) const {
  std::vector<std::pair<PHLWINDOW, LRect>> boxes;
  boxes.reserve(m_tiles.size());
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    if (const auto win = m_tiles[i].win.lock(); win && win != exclude)
      boxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));
  }
  return boxes;
}

// After a rebuild+relayout: freeze every tile at the box it is shown at right
// now (from `oldBoxes`, captured before the rebuild; empty = keep the freshly
// assigned naturals, i.e. a plain open starts from the real boxes) and restart
// the tile clock. The next frames glide into the fresh targets.
void Overview::startTileGlide(
    const std::vector<std::pair<PHLWINDOW, LRect>> &oldBoxes) {
  bool newcomers = false;
  if (!oldBoxes.empty()) {
    for (auto &t : m_tiles) {
      t.natural = t.target;
      bool matched = false;
      for (const auto &[oldWin, oldBox] : oldBoxes)
        if (oldWin == t.win.lock()) {
          t.natural = oldBox;
          matched = true;
          break;
        }
      t.appear = matched ? 1.0 : 0.0;
      newcomers |= !matched;
    }
    // ghosts: EVERY tile the rebuild removed fades/scales out where it was —
    // including all->one collapse, where the disappearing half of the grid
    // must animate out, not vanish one frame. (Windows are still alive
    // behind the opaque backdrop; the soft translucent render keeps it
    // reading as dispersal.)
    m_ghosts.clear();
    for (const auto &[oldWin, oldBox] : oldBoxes) {
      bool kept = false;
      for (auto &t : m_tiles)
        if (t.win.lock() == oldWin) { kept = true; break; }
      if (!kept)
        m_ghosts.push_back(model::Ghost{oldWin, oldBox});
    }
  } else
    for (auto &t : m_tiles)
      t.appear = 1.0;
  m_tileClock.begin();
  if (newcomers || !m_ghosts.empty()) {
    m_populate.begin();
    ensureAnimPump(); // frames must keep coming while population runs
  }
}

void Overview::replayReflow(
    std::vector<std::pair<PHLWINDOW, LRect>> &oldBoxes) {
  m_hovered = m_hoveredStrip = -1;
  buildTiles();
  buildStrip();
  layoutTiles();
  startTileGlide(oldBoxes);
  if (m_selected >= static_cast<int>(m_tiles.size()))
    m_selected = m_tiles.empty() ? -1 : static_cast<int>(m_tiles.size()) - 1;
  m_progress = 1.0;
  m_opening = true;
  m_timeline.pinEnd(animDuration()); // chrome settled; only the tiles glide
  damage();
}

void Overview::syncTiles() {
  if (!m_active || !m_opening || m_pendingDeactivate)
    return;
  const auto ws = m_workspace.lock();
  if (!ws)
    return;

  // The displayed window set can change while up (window closes, or opens/moves
  // onto this workspace/monitor). Detect any add/remove with tileBelongs() —
  // the SAME predicate buildTiles() uses, so the count settles in one frame
  // (else reflow every frame, the expo-mode churn bug) — then glide via
  // replayReflow (chrome stays at 1).
  const auto m = m_monitor.lock();
  const auto belongs = [&](const PHLWINDOW &w) {
    return tileBelongs(w, m, ws);
  };
  size_t expected = 0;
  for (const auto &w : Desktop::windowState()->windows())
    if (belongs(w))
      ++expected;

  bool diff = expected != m_tiles.size();
  if (!diff)
    for (const auto &t : m_tiles) {
      const auto w = t.win.lock();
      if (!belongs(w)) { // a tracked window died or left this workspace
        diff = true;
        break;
      }
    }
  if (!diff)
    return;

  auto oldBoxes = captureCurrentBoxes();

  // Desktop mode: adding/removing a window must NOT shuffle the others.
  // buildTiles() carries every parked target across the rebuild, so survivors
  // stay put automatically; only the newcomer flows to its real scaled spot.
  if (m_desktopMode)
    for (auto &t : m_tiles)
      t.parked = true;

  replayReflow(oldBoxes);
}

void Overview::restoreFill() {
  for (const auto &w : Desktop::windowState()->windows())
    if (w && w->wlSurface())
      w->wlSurface()->m_fillIgnoreSmall = false;
}

// Fade out bars/popups so they don't bleed through the translucent backdrop:
// stash each layer surface's alpha goal and drive it to 0; restoreLayers()
// animates them back. Fully reversible (nothing persists past close); works for
// any layer-shell client.
void Overview::hideLayers() {
  const auto m = m_monitor.lock();
  if (!m)
    return;
  const bool top = cfg::layer.hide_top != 0;
  const bool ovl = cfg::layer.hide_overlay != 0;
  if (!top && !ovl)
    return;
  const auto fade = [this](const std::vector<PHLLSREF> &layer) {
    for (const auto &ref : layer) {
      const auto ls = ref.lock();
      if (!ls)
        continue;
      auto &a = ls->alpha()[Desktop::View::LS_ALPHA_FADE];
      if (!a)
        continue;
      if (isAboveLayer(ls->m_namespace))
        continue; // keep above-overview surfaces fully visible
      m_hiddenLayers.emplace_back(ref, a->goal());
      *a = 0.F;
    }
  };
  if (top)
    fade(m->m_layerSurfaceLayers[2]); // ZWLR_LAYER_SHELL_V1_LAYER_TOP
  if (ovl)
    fade(m->m_layerSurfaceLayers[3]); // ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
  if (!m_hiddenLayers.empty()) {
    debug::dbg("hid " + std::to_string(m_hiddenLayers.size()) + " layer surface(s)");
    damage();
  }
}

void Overview::restoreLayers() {
  for (auto &[ref, alpha] : m_hiddenLayers)
    if (const auto ls = ref.lock(); ls) {
      auto &a = ls->alpha()[Desktop::View::LS_ALPHA_FADE];
      if (a)
        *a = alpha;
    }
  m_hiddenLayers.clear();
}

} // namespace gloview
