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
// The modifier bit a pressed key ITSELF contributes (evdev codes), so bare
// modifier bindings can mask their own bit out of the held-mods state before
// matching.
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

void Overview::onKey(const IKeyboard::SKeyEvent &e, bool &cancel) {
  if (!m_active)
    return;
  // Keyboard model: the overview consumes ESC/TAB (dismiss), Enter (focus
  // selection), arrows (move cursor); everything else passes THROUGH to
  // Hyprland in passthrough mode so the user's normal keybinds keep working.
  // With passthrough off it's fully modal — every key swallowed. Keycodes are
  // evdev (layout-independent).
  const bool passthrough = cfg::behavior.passthrough_keys != 0;

  if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED) {
    // Releasing the configured modifier while an alt-tab cycle is active
    // commits the current selection — focuses it and dismisses, exactly like
    // letting go of Alt in a normal alt-tab. Off (alt_tab_commit_on_release=0):
    // releasing does nothing: the selection just stays put until an explicit
    // key_activate/click, for people who find release-triggers-action too
    // eager. modBitForKeycode is used (not keyNameToCodes/ modNameToBit — both
    // defined further down this file) so this stays a simple keycode→ bit
    // comparison with no forward-declaration to worry about.
    if (m_altTabbing &&
        cfg::keys.alt_tab_commit_on_release != 0) {
      const std::string modName =
          cfg::keys.alt_tab_modifier.get();
      uint32_t wantBit = HL_MODIFIER_ALT;
      if (modName == "super" || modName == "meta" || modName == "win")
        wantBit = HL_MODIFIER_META;
      else if (modName == "ctrl" || modName == "control")
        wantBit = HL_MODIFIER_CTRL;
      else if (modName == "shift")
        wantBit = HL_MODIFIER_SHIFT;
      if (modBitForKeycode(e.keycode) == wantBit) {
        m_altTabbing = false;
        activateSelection();
        cancel = true;
        return;
      }
    }
    cancel = !passthrough; // balance the release half of any key we let through
                           // on press
    return;
  }

  // Each action binds a config list of key NAMES
  // (esc/tab/enter/left/shift/hjkl/…; bare digit = number-row key), optionally
  // with modifier prefixes ("shift+tab"). key_* = "" disables the action (key
  // falls through).
  const int k = e.keycode;
  // Held modifiers across all keyboards (not just keys pressed since the
  // overview opened), minus the pressed key's OWN modifier bit — else a bare
  // modifier name
  // ("shift", the key_desktop default) could never match its own press.
  uint32_t mods = g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
  mods &= ~modBitForKeycode(k);
  bool handled = true;
  if (keyMatches(k, mods, cfg::keys.close.get()))
    close();
  else if (keyMatches(k, mods, cfg::keys.next_workspace.get()))
    stepWorkspace(
        1); // cycle the displayed workspace card (wraps; committed on close)
  else if (keyMatches(k, mods, cfg::keys.prev_workspace.get()))
    stepWorkspace(-1);
  else if (keyMatches(k, mods, cfg::keys.activate.get()))
    activateSelection();
  else if (keyMatches(k, mods, cfg::keys.close_window.get()))
    closeTileWindow(m_selected); // sendClose the SELECTED tile (keyboard/hover
                                 // cursor), stay open & reflow
  else if (keyMatches(k, mods, cfg::keys.left.get()))
    moveSelection(-1, 0);
  else if (keyMatches(k, mods, cfg::keys.right.get()))
    moveSelection(1, 0);
  else if (keyMatches(k, mods, cfg::keys.up.get()))
    moveSelection(0, -1);
  else if (keyMatches(k, mods, cfg::keys.down.get()))
    moveSelection(0, 1);
  else if (keyMatches(k, mods, cfg::keys.desktop.get()))
    setDesktopMode(!m_desktopMode);
  else if (keyMatches(k, mods, cfg::keys.all_workspaces.get()))
    toggleAllWorkspaces(); // flip the expo (all-workspaces) main view
  // Ctrl is masked out of the match here (not treated as a strict combo
  // requirement): in "jump" mode it's the escape hatch back to the old
  // stay-open behavior, so a bare digit and Ctrl+digit both need to hit this
  // branch — see the mode check below.
  else if (const int idx =
               keyIndex(k, mods & ~HL_MODIFIER_CTRL, cfg::keys.workspace.get());
           idx >= 0) {
    // Key position N (0-based) maps DIRECTLY to workspace N+1 — "0" is always
    // workspace 10 — independent of what's currently on the strip, and creates
    // the workspace first if it doesn't exist yet (same as clicking "+").
    // Previously this walked the (non-+/All) strip cards by POSITION, so it
    // only ever reached workspaces that already happened to have a card up,
    // i.e. ones already visited this session — pressing "0" did nothing at all
    // for a never-opened workspace 10.
    const int id = idx + 1;
    const auto m = m_monitor.lock();
    auto ws = State::workspaceState()->query().id(id).run();
    const bool justCreated = !ws && m;
    if (justCreated)
      ws = State::workspaceState()->create(id, m->m_id, "", false);
    if (ws) {
      if (justCreated) {
        // A brand-new empty workspace is reaped within a frame or two unless
        // held — same guard addWorkspace() uses for "+". deactivate() releases
        // it later.
        ws->setPersistent(true);
        m_newWorkspaces.push_back(ws);
        m_newCardId =
            id; // pop-in animation for the freshly created card, like "+"
        m_newCard.begin();
        m_newCardAnim = true;
      }
      model::StripItem it;
      it.ws = ws;
      switchToWorkspace(it);
      if (m) {
        if (const auto target = m_workspace.lock();
            target && target != m->m_activeWorkspace) {
          // Tried internal=true here first to stop Hyprland's animated slide
          // transition from leaking a fullscreen window's content into our
          // backdrop for the duration of the switch (repro: fullscreen
          // client on the target ws, switch to it via THIS digit-key path
          // only — never via an external `hyprctl dispatch workspace` /
          // native keybind switch while the overview is up). That stopped
          // the leak but broke something else: a fullscreen window on the
          // target workspace stopped rendering at all afterward — bare
          // desktop/wallpaper, window still alive, only recovering after
          // toggling the real desktop workspace back and forth via a native
          // keybind. So `internal` gates more than just the visual
          // transition.
          //
          // Also: per the comment on deactivate()'s own changeWorkspace()
          // call below, `internal` isn't even the right lever regardless —
          // changeWorkspace() ALWAYS starts Hyprland's native slide (its
          // calls into the animation manager hardcode instant=false, not
          // exposed through changeWorkspace()'s own signature), so
          // internal=true was never going to stop the transition itself
          // either.
          //
          // Fix, mirroring deactivate()'s own changeWorkspace() call
          // (same rationale, see its comment): keep internal=false so every
          // bit of real-switch bookkeeping runs (this is what brought the
          // fullscreen window back), let the transition start normally, then
          // instantly finish it by warping both workspaces' m_alpha /
          // m_renderOffset straight to their already-assigned goals. The
          // switch still fully happens — nothing here skips it — it just
          // doesn't visibly slide over the following frames, so there's no
          // window of time where Hyprland's own transition-render can put
          // the target workspace's content on screen outside our own render
          // pass (and outside shouldRenderWindow's reach) while the overview
          // is still up.
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
      // "switch" (default): stay open, always — Ctrl is a no-op either way.
      // "jump": a bare digit also closes the overview immediately (no Enter
      // needed); Ctrl+digit falls back to the old stay-open behavior.
      const bool jumpMode =
          cfg::behavior.workspace_key_mode.get() == "jump";
      const bool ctrlHeld = (mods & HL_MODIFIER_CTRL) != 0;
      if (jumpMode && !ctrlHeld)
        close();
    }
  } else
    handled = false;
  cancel = handled || !passthrough;
}

namespace {
// Split a config string into key tokens on commas / whitespace.
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

// Resolve a key NAME (case-insensitive) to its evdev keycode(s). Bare digit =
// number-row key; left/right-variant names (shift/ctrl/alt/super) and enter
// resolve to BOTH codes.
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
      // letters (evdev rows; lets users bind hjkl / wasd etc.)
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

// Modifier NAME (a "+"-prefix in a combo token) → HL_MODIFIER bit; 0 = not a
// modifier.
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

// Match one token ("tab", "shift+tab", "ctrl+shift+k") against the pressed
// keycode and the currently held modifiers — EXACT on shift/ctrl/alt/super.
// Unrequested modifiers must NOT be held: a bare "tab" must not swallow
// shift+tab (its own action) nor super+tab (commonly the user's gloview:toggle
// bind — with passthrough it falls through to Hyprland's keybind manager, so
// the same bind that opened the overview closes it). Only lock states
// (caps/num) are ignored.
bool comboMatches(int keycode, uint32_t heldMods, std::string token) {
  constexpr uint32_t STRICTMODS =
      HL_MODIFIER_SHIFT | HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META;

  uint32_t need = 0;
  size_t pos;
  while ((pos = token.find('+')) != std::string::npos) {
    const uint32_t bit = modNameToBit(token.substr(0, pos));
    if (bit == 0)
      return false; // unknown modifier name → the token can never match
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

// 0-based token position of keycode within the list, else -1. The number row
// uses it in onKey to map directly to a workspace ID (idx+1) — see the
// key_workspace handler.
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
  if (m_tiles.empty())
    return;
  if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size())) {
    m_selected = (m_hovered >= 0) ? m_hovered : 0;
    syncFocus();
    damage();
    return;
  }
  const LRect cur = currentBox(m_tiles[m_selected], m_selected);
  const double cx = cur.cx(), cy = cur.cy();
  int best = -1;
  double bestScore = 1e18;
  for (size_t i = 0; i < m_tiles.size(); ++i) {
    if (static_cast<int>(i) == m_selected)
      continue;
    const LRect b = currentBox(m_tiles[i], static_cast<int>(i));
    const double ddx = b.cx() - cx;
    const double ddy = b.cy() - cy;
    const double along =
        dx * ddx + dy * ddy; // distance in the requested direction
    if (along <= 1.0)
      continue;                                        // not in that direction
    const double perp = std::abs(dx * ddy - dy * ddx); // lateral offset
    const double score =
        along + perp * 2.0; // prefer aligned, penalize sideways drift
    if (score < bestScore) {
      bestScore = score;
      best = static_cast<int>(i);
    }
  }
  if (best >= 0) {
    m_selected = best;
    syncFocus();
    damage();
  }
}

// Advance the Alt-Tab cursor by dir (+1/-1). Only ever called for an
// ALREADY-active session (the first "tab" of a session is handled directly in
// altTabInvoke, which lands on tile 0). The visiting order is simply the tile
// index — no separate order/position bookkeeping needed.
void Overview::stepAltTab(int dir) {
  const int n = static_cast<int>(m_tiles.size());
  if (n == 0)
    return;
  m_selected = ((m_selected + dir) % n + n) % n;
  syncFocus();
  damage();
}

void Overview::activateSelection() {
  PHLWINDOW w;
  if (m_selected >= 0 && m_selected < static_cast<int>(m_tiles.size()))
    w = m_tiles[m_selected].win.lock();
  focusAndClose(w, Desktop::FOCUS_REASON_KEYBIND);
}

// Point Hyprland's REAL focus at the selected tile while the overview is up.
// passthrough keybinds like `killactive` act on the focused window; without
// this, focus stayed on whatever was focused before open, so a hotkey hit the
// WRONG window (ring and real focus diverged). Keep focus in lockstep with
// m_selected.
//
// Guarded to the monitor's ACTIVE workspace: a displayed (uncommitted)
// workspace's tiles are hidden, so focusing one would desync focus from the
// live desktop without a real switch. fullWindowFocus does NOT change the
// active workspace, so same-workspace only moves input focus.
void Overview::syncFocus() const {
  if (!m_active || m_selected < 0 ||
      m_selected >= static_cast<int>(m_tiles.size()))
    return;
  const auto m = m_monitor.lock();
  const auto w = m_tiles[m_selected].win.lock();
  if (!m || !w || !w->m_isMapped || w->isHidden())
    return;
  if (w->m_workspace !=
      m->m_activeWorkspace) // displaying a non-live workspace — don't desync
    return;
  Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_KEYBIND);
}

} // namespace gloview
