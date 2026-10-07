#include "../config/config.hpp"
#include "../overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {
uint32_t modBitForKeycode(int kc) {
  switch (kc) {
  case 42:
  case 54:
    return HL_MODIFIER_SHIFT;
  case 29:
  case 97:
    return HL_MODIFIER_CTRL;
  case 56:
  case 100:
    return HL_MODIFIER_ALT;
  case 125:
  case 126:
    return HL_MODIFIER_META;
  default:
    return 0;
  }
}
} // namespace

uint32_t Overview::altTabModBit() const {
  const std::string modName = cfg::keys.alt_tab_modifier.get();
  if (modName == "super" || modName == "meta" || modName == "win")
    return HL_MODIFIER_META;
  if (modName == "ctrl" || modName == "control")
    return HL_MODIFIER_CTRL;
  if (modName == "shift")
    return HL_MODIFIER_SHIFT;
  return HL_MODIFIER_ALT;
}

void Overview::onKey(const IKeyboard::SKeyEvent &e, bool &cancel) {
  if (!m_active)
    return;
  const bool passthrough = cfg::behavior.passthrough_keys != 0;

  if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED) {
    // The modifier coming up is swallowed either way — letting it through fires the
    // user's own SUPER binding and tears the state down before the run settles.
    if (m_altTabbing && modBitForKeycode(e.keycode) == altTabModBit()) {
      m_altTabbing = false;
      // A hold commits here; too early to apply means arming it, as a tap does.
      if (cfg::keys.alt_tab_commit_on_release != 0 && !m_commitPending) {
        if (entrySettled())
          activateSelection();
        else
          m_commitPending = true;
      }
      cancel = true;
      return;
    }
    cancel = !passthrough;
    return;
  }

  const int k = e.keycode;
  // Held mods minus the pressed key's own bit, or a bare modifier name never matches.
  uint32_t mods = g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
  mods &= ~modBitForKeycode(k);
  bool handled = true;
  if (keyMatches(k, mods, cfg::keys.close.get()))
    close();
  else if (keyMatches(k, mods, cfg::keys.next_workspace.get()))
    stepWorkspace(
        1);
  else if (keyMatches(k, mods, cfg::keys.prev_workspace.get()))
    stepWorkspace(-1);
  else if (keyMatches(k, mods, cfg::keys.activate.get()))
    activateSelection();
  else if (keyMatches(k, mods, cfg::keys.close_window.get()))
    closeTileWindow(m_scene.selected());
  else if (keyMatches(k, mods, cfg::keys.left.get()))
    moveSelection(-1, 0);
  else if (keyMatches(k, mods, cfg::keys.right.get()))
    moveSelection(1, 0);
  else if (keyMatches(k, mods, cfg::keys.up.get()))
    moveSelection(0, -1);
  else if (keyMatches(k, mods, cfg::keys.down.get()))
    moveSelection(0, 1);
  else if (keyMatches(k, mods, cfg::keys.desktop.get()))
    setDesktopMode(!m_view.desktop());
  else if (keyMatches(k, mods, cfg::keys.all_workspaces.get()))
    toggleAllWorkspaces();
  else if (keyMatches(k, mods, cfg::keys.carousel.get()))
    toggleCarousel();
  // Ctrl masked out: bare digit and Ctrl+digit both land here.
  else if (const int idx =
               keyIndex(k, mods & ~HL_MODIFIER_CTRL, cfg::keys.workspace.get());
           idx >= 0) {
    const int id = idx + 1;
    const auto m = m_monitor.lock();
    auto ws = State::workspaceState()->query().id(id).run();
    const bool justCreated = !ws && m;
    if (justCreated)
      ws = State::workspaceState()->create(id, m->m_id, "", false);
    if (ws) {
      if (justCreated) {
        // Persistent, or a new empty workspace is reaped within a frame.
        ws->setPersistent(true);
        m_fx.addNewWorkspace(ws);
        m_newCardId =
            id;
        m_clocks.newCard.begin();
        m_newCardAnim = true;
      }
      model::StripItem it;
      it.ws = ws;
      const bool jumpMode = cfg::behavior.workspace_key_mode.get() == "jump";
      const bool ctrlHeld = (mods & HL_MODIFIER_CTRL) != 0;
      const bool doJump   = jumpMode && !ctrlHeld;
      const bool instant  = doJump && anim::jumpStyle() == "instant";
      switchToWorkspace(it, instant);
      if (m) {
        if (const auto target = m_workspace.lock();
            target && target != m->m_activeWorkspace) {
          // internal=true is not the lever (it stopped a fullscreen window on
          // the target from rendering); keep it false and warp the slide away below.
          const auto oldWs = m->m_activeWorkspace; // capture BEFORE the switch
          m->changeWorkspace(target, false, true, false);
          if (oldWs && oldWs != target) {
            oldWs->m_alpha->setValueAndWarp(oldWs->m_alpha->goal());
            oldWs->m_renderOffset->setValueAndWarp(oldWs->m_renderOffset->goal());
          }
          target->m_alpha->setValueAndWarp(target->m_alpha->goal());
          target->m_renderOffset->setValueAndWarp(target->m_renderOffset->goal());
          m_liveWsAtOpen = m->m_activeWorkspace;
        }
      }
      // "switch" stays open; "jump" closes now, or when the ws-slide settles.
      if (doJump) {
        if (instant)
          close();
        else
          m_pendingJumpClose = true;
      }
    }
  } else
    handled = false;
  cancel = handled || !passthrough;
}

namespace {
std::vector<std::string> keyTokens(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else
      cur.push_back(c);
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

const std::vector<int> &keyNameToCodes(std::string t) {
  static const std::vector<int> NONE;
  for (auto &c : t)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const std::unordered_map<std::string, std::vector<int>> M = {
      {"0", {11}},
      {"1", {2}},
      {"2", {3}},
      {"3", {4}},
      {"4", {5}},
      {"5", {6}},
      {"6", {7}},
      {"7", {8}},
      {"8", {9}},
      {"9", {10}},
      {"grave", {41}},
      {"tilde", {41}},
      {"`", {41}},
      {"esc", {1}},
      {"escape", {1}},
      {"tab", {15}},
      {"space", {57}},
      {"enter", {28, 96}},
      {"return", {28, 96}},
      {"kpenter", {96}},
      {"backspace", {14}},
      {"delete", {111}},
      {"del", {111}},
      {"insert", {110}},
      {"left", {105}},
      {"right", {106}},
      {"up", {103}},
      {"down", {108}},
      {"home", {102}},
      {"end", {107}},
      {"pageup", {104}},
      {"pagedown", {109}},
      {"shift", {42, 54}},
      {"lshift", {42}},
      {"rshift", {54}},
      {"ctrl", {29, 97}},
      {"control", {29, 97}},
      {"lctrl", {29}},
      {"rctrl", {97}},
      {"alt", {56, 100}},
      {"lalt", {56}},
      {"ralt", {100}},
      {"super", {125, 126}},
      {"meta", {125, 126}},
      {"win", {125, 126}},
      {"f1", {59}},
      {"f2", {60}},
      {"f3", {61}},
      {"f4", {62}},
      {"f5", {63}},
      {"f6", {64}},
      {"f7", {65}},
      {"f8", {66}},
      {"f9", {67}},
      {"f10", {68}},
      {"f11", {87}},
      {"f12", {88}},
      {"a", {30}},
      {"b", {48}},
      {"c", {46}},
      {"d", {32}},
      {"e", {18}},
      {"f", {33}},
      {"g", {34}},
      {"h", {35}},
      {"i", {23}},
      {"j", {36}},
      {"k", {37}},
      {"l", {38}},
      {"m", {50}},
      {"n", {49}},
      {"o", {24}},
      {"p", {25}},
      {"q", {16}},
      {"r", {19}},
      {"s", {31}},
      {"t", {20}},
      {"u", {22}},
      {"v", {47}},
      {"w", {17}},
      {"x", {45}},
      {"y", {21}},
      {"z", {44}},
  };
  const auto it = M.find(t);
  return it != M.end() ? it->second : NONE;
}

uint32_t modNameToBit(std::string t) {
  for (auto &c : t)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "shift")
    return HL_MODIFIER_SHIFT;
  if (t == "ctrl" || t == "control")
    return HL_MODIFIER_CTRL;
  if (t == "alt")
    return HL_MODIFIER_ALT;
  if (t == "super" || t == "meta" || t == "win")
    return HL_MODIFIER_META;
  return 0;
}

// Extra held modifiers fail the match: a bare "tab" must not swallow
// shift+tab or the user's own toggle bind.
bool comboMatches(int keycode, uint32_t heldMods, std::string token) {
  constexpr uint32_t STRICTMODS =
      HL_MODIFIER_SHIFT | HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META;

  uint32_t need = 0;
  size_t pos;
  while ((pos = token.find('+')) != std::string::npos) {
    const uint32_t bit = modNameToBit(token.substr(0, pos));
    if (bit == 0)
      return false;
    need |= bit;
    token.erase(0, pos + 1);
  }

  bool codeHit = false;
  for (const int c : keyNameToCodes(token))
    if (c == keycode) {
      codeHit = true;
      break;
    }
  if (!codeHit)
    return false;
  if ((heldMods & need) != need)
    return false;
  return (heldMods & STRICTMODS & ~need) == 0;
}
} // namespace

bool Overview::keyMatches(int keycode, uint32_t mods,
                          const std::string &combo) const {
  for (const auto &tok : keyTokens(combo))
    if (comboMatches(keycode, mods, tok))
      return true;
  return false;
}

int Overview::keyIndex(int keycode, uint32_t mods,
                       const std::string &combo) const {
  int idx = 0;
  for (const auto &tok : keyTokens(combo)) {
    if (comboMatches(keycode, mods, tok))
      return idx;
    ++idx;
  }
  return -1;
}

void Overview::moveSelection(int dx, int dy) {
  if (m_scene.tiles().empty())
    return;
  if (!m_scene.hasSelection()) {
    m_scene.select((m_scene.hoveredTile() >= 0) ? m_scene.hoveredTile() : 0);
    damage();
    return;
  }
  // Carousel: dx is a circular index step, dy is unused; relayoutCarousel()
  // reacts to m_scene.selected().
  if (carouselMode()) {
    if (dx != 0) {
      m_scene.stepSelection(dx);
      damage();
    }
    return;
  }
  const LRect cur = currentBox(m_scene.tiles()[m_scene.selected()], m_scene.selected());
  const double cx = cur.cx(), cy = cur.cy();
  int best = -1;
  double bestScore = 1e18;
  for (size_t i = 0; i < m_scene.tiles().size(); ++i) {
    if (static_cast<int>(i) == m_scene.selected())
      continue;
    const LRect b = currentBox(m_scene.tiles()[i], static_cast<int>(i));
    const double ddx = b.cx() - cx;
    const double ddy = b.cy() - cy;
    const double along =
        dx * ddx + dy * ddy;
    if (along <= 1.0)
      continue;
    const double perp = std::abs(dx * ddy - dy * ddx);
    const double score =
        along + perp * 2.0;
    if (score < bestScore) {
      bestScore = score;
      best = static_cast<int>(i);
    }
  }
  if (best >= 0) {
    m_scene.select(best);
    damage();
  }
}

// Steps through the order the user SEES. In the run that is the slot order, not the
// scene's array order: the cards are laid out most-recently-used first, so stepping
// the array index would move the selection to a window that is not the next card on
// screen. A grid has no slot order and keeps the array order it is drawn in.
void Overview::stepAltTab(int dir) {
  const int n = static_cast<int>(m_scene.size());
  if (n == 0)
    return;
  const auto &slot = m_view.slot();
  if (carouselMode() && m_scene.hasSelection() &&
      slot.size() == static_cast<std::size_t>(n)) {
    const int cur = m_view.slotOf(static_cast<std::size_t>(m_scene.selected()));
    if (cur >= 0) {
      const int nxt = ((cur + dir) % n + n) % n;
      m_scene.select(static_cast<int>(slot[static_cast<std::size_t>(nxt)]));
      damage();
      return;
    }
  }
  m_scene.stepSelection(dir);
  damage();
}

void Overview::activateSelection() {
  PHLWINDOW w;
  if (m_scene.selected() >= 0 && m_scene.selected() < static_cast<int>(m_scene.tiles().size()))
    w = m_scene.tiles()[m_scene.selected()].win.lock();
  focusAndClose(w, Desktop::FOCUS_REASON_KEYBIND);
}


} // namespace gloview
