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
  // parked boxes survive the rebuild
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

  for (const auto &w : Desktop::windowState()->windows()) {
    if (!tileBelongs(w, m, ws))
      continue;

    model::Tile t;
    t.win = w;
    if (const auto it = priorTargets.find(w); it != priorTargets.end()) {
      t.target = it->second.first;
      t.parked = true;
    }
    // goal(), not value(): value() carries the slide offset
    const auto p = w->positionAnimation()->goal();
    const auto s = w->sizeAnimation()->goal();
    t.natural = LRect{p.x - m->m_position.x, p.y - m->m_position.y,
                      std::max(1.0, s.x), std::max(1.0, s.y)};
    m_tiles.push_back(t);
  }

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
    // glyph pre-rendered: text rendering mid-pass is unsafe
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
      real;
  const int curId = (cur && !cur->m_isSpecialWorkspace) ? cur->m_id : -1;
  int highestId = 0;
  for (const auto &wref : State::workspaceState()->workspaces()) {
    const auto ws = wref.lock();
    if (!ws || ws->m_monitor.lock() != m)
      continue;
    if (ws->m_isSpecialWorkspace) {
      if (showSpecial &&
          wsHasWindows(ws)) {
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
  // special cards bypass the numeric ordering
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
    if (curId > 0) {
      if (curId - 1 > 0)
        idsToShow.insert(curId - 1);
      idsToShow.insert(curId + 1);
    }
  } else {
    const int upTo = std::max({10, highestId, curId});
    for (int id = 1; id <= upTo; ++id)
      idsToShow.insert(id);
    for (const auto &[id, ws] :
         real)
      idsToShow.insert(id);
  }
  if (curId > 0)
    idsToShow.insert(
        curId);

  // leading All-workspaces card; Kind::All guarded elsewhere
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
      it.virtualWs = true; // created on click/drop
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
  for (auto &sc : specialCards)
    m_strip.push_back(std::move(sc));

  model::StripItem plus;
  plus.kind = model::StripItem::Kind::Plus;
  plus.id = 0;
  m_strip.push_back(std::move(plus));

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

  const double cellMain = horiz ? cardW : (cardH + labelH);
  const int n = static_cast<int>(m_strip.size());
  const double bandMain = horiz ? band.w : band.h;
  const double availMain = std::max(1.0, bandMain - 2 * margin);
  const double groupMain = n * cellMain + std::max(0, n - 1) * gap;
  const double mainOrigin = (horiz ? band.x : band.y) + margin;

  m_stripScrollMax = std::max(0.0, groupMain - availMain);
  const double start = (m_stripScrollMax <= 0.0)
                           ? mainOrigin + (availMain - groupMain) / 2.0
                           : mainOrigin;

  const double cardX = band.x + margin;
  const double cardY = band.y + margin + labelH;

  double main = start;
  for (auto &it : m_strip) {
    if (horiz)
      it.card = LRect{main, cardY, cardW, cardH};
    else
      it.card = LRect{cardX, main + labelH, cardW, cardH};
    main += cellMain + gap;
  }

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
      stripSlide(eased()); // draw and hit-test must share this slide
  return LRect{c.x + slide.x + scroll.x, c.y + slide.y + scroll.y, c.w, c.h};
}

// `card` is already scroll/slide-adjusted.
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

  // The renderer asks m_engine / m_run, never the config: free-arrange, the
  // all/one flip and the carousel switch all override what `layout` says, and a
  // renderer that read the config would draw an arrangement this function never
  // produced.
  m_engine = m_desktopMode ? Engine::Natural
                           : parseEngine(cfg::grid.layout.get().c_str());
  // The run is a separate mechanism over any layout, and only in the
  // all-workspaces view: a single workspace's handful of tiles has nothing to
  // arrange against. With it off the layout runs untouched.
  m_run = cfg::carousel.enabled != 0 && !m_desktopMode && showAllWorkspaces();

  LayoutCfg cfg;
  cfg.engine = m_engine;
  cfg.gap = cfg::grid.gap;
  const int padX = cfg::grid.padding;
  const int padT = cfg::grid.padding_top;
  const int padB = cfg::grid.padding_bottom;
  cfg.padLeft = padX;
  cfg.padRight = padX;
  cfg.padTop = padT;
  cfg.padBottom = padB;
  // reserve the band footprint on its anchored edge
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

  // whole monitor scaled into the usable box
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
      if (t.parked)
        continue;
      t.target = LRect{m_desktopOx + t.natural.x * s,
                       m_desktopOy + t.natural.y * s,
                       std::max(1.0, t.natural.w * s),
                       std::max(1.0, t.natural.h * s)};
    }
    return;
  }
  m_carousel        = {};
  m_carouselFor     = -1;

  // The run replaces the main area wholesale: it lays one window against its
  // neighbours, which a grid of boxes cannot express.
  // Slots are built AROUND m_selected: selection = relayout, not repaint.
  if (m_run) {
    const int sel = (m_selected >= 0 && m_selected < static_cast<int>(m_tiles.size()))
                        ? m_selected
                        : 0;
    CarouselMetrics mt;
    mt.visibleCards = std::max(1, static_cast<int>(cfg::carousel.cards.get()));
    if (const int w = static_cast<int>(cfg::carousel.card_width.get());
        w > 0) {
      mt.selectedW = w;
      mt.overlap   = w * 60.0 / 500.0;
    }
    const LRect usable{cfg.padLeft, cfg.padTop,
                       std::max(1.0, m->m_size.x - cfg.padLeft - cfg.padRight),
                       std::max(1.0, m->m_size.y - cfg.padTop - cfg.padBottom)};
    m_carousel    = computeCarousel(static_cast<int>(m_tiles.size()), sel,
                                    usable, mt);
    m_carouselFor = sel;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
      const auto &c = m_carousel.cards[i];
      // Zero target = off-run. Nothing paints an off-run card, so where it would
      // have gone does not matter; a zeroed box keeps the glide from resizing it.
      m_tiles[i].target = c.visible ? LRect{usable.x + m_carousel.originX + c.x,
                                            usable.y + m_carousel.originY +
                                                m_carousel.stripY + c.y,
                                            c.w, c.h}
                                    : LRect{};
    }
    return;
  }
  m_carousel        = {};
  m_carouselFor     = -1;

  std::vector<LRect> nat;
  nat.reserve(m_tiles.size());
  for (const auto &t : m_tiles)
    nat.push_back(t.natural);

  const auto out =
      computeLayout(nat, LRect{0, 0, m->m_size.x, m->m_size.y}, cfg);
  for (size_t i = 0; i < m_tiles.size(); ++i)
    m_tiles[i].target = out[i];
}

// Survivors glide from their captured boxes; no chrome re-reveal.
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

// Freeze tiles at their shown boxes (`oldBoxes`, pre-rebuild); empty = keep
    // the fresh naturals, so a plain open starts from the real boxes.
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
    // Every removed tile fades out where it was — never vanishes one frame.
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
  m_tileClockMs    = m_glide.ms;    // snapshot: glideDur() must return THIS value
  m_tileClockCurve = m_glide.curve; // snapshot: currentBox() must use THIS curve
  if (newcomers || !m_ghosts.empty()) {
    m_rebuildClock.begin();
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
  m_timeline.pinEnd(animDuration());
  damage();
}

void Overview::syncTiles() {
  if (!m_active || !m_opening || m_pendingDeactivate)
    return;
  const auto ws = m_workspace.lock();
  if (!ws)
    return;

  // Add/remove detection MUST reuse buildTiles()'s predicate (tileBelongs).
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
      if (!belongs(w)) {
        diff = true;
        break;
      }
    }
  if (!diff)
    return;

  auto oldBoxes = captureCurrentBoxes();

  // NOT parking here. `parked` means "the user dropped this window here, keep it",
  // and only the drop path may set it: parking every survivor made
  // layoutTiles' desktop branch skip the whole set, so free-arrange froze after
  // the first sync and the windows stayed wherever the previous layout left them.
  replayReflow(oldBoxes);
}

void Overview::restoreFill() {
  for (const auto &w : Desktop::windowState()->windows())
    if (w && w->wlSurface())
      w->wlSurface()->m_fillIgnoreSmall = false;
}

// Fade bars/popups to 0 so they don't bleed through the translucent backdrop.
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
