#pragma once

#include <cstdint>

#include <unordered_map>
#include <string>

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace Render {
class ITexture;
class IFramebuffer;
} // namespace Render

namespace gloview::pixel {

// Entries with no object behind them still need a key. They live in a map of their
// own and are never swept, so any distinct integer works; naming the slots keeps a
// call site from passing a bare literal that reads like a window address.
enum Slot : int { All = -2, CarouselCount = -1 };

// Captions, the close glyph, entry snapshots.
//
// The KEYING POLICY is the whole job: a caption is asked for whole and cut to the
// card, and sharing a slot measures one texture and draws the other.
//
// Entries are owned by what they describe. A caption or snapshot keyed by a raw
// address outlives the object at it whenever the scene is not rebuilt in between:
// Hyprland recycles addresses, so the next window to land on a closed window's
// address inherits its texture -- in snapshot mode, its contents. Storing a weak
// reference beside the key removes that by construction: once the object dies the
// reference expires and the lookup misses, with no sweep anywhere to forget to run.
// Entries with no object behind them (the strip's "All" card, the carousel counter)
// have nothing to die and live in m_fixed for the length of the session.
class PixelCache {
  public:
    struct LabelEntry {
      std::string text;
      int maxW = 0;
      int size = 0;
      // Colour and size belong to the key, not just the string: the config is live
      // (hl.config), and a caption baked in the old colour keeps being drawn after
      // the user changes it. Same defect the carousel counter had.
      CHyprColor col;
      SP<Render::ITexture> tex;

      [[nodiscard]] bool matches(const std::string &t, int w, int s,
                                 const CHyprColor &c) const {
        return tex && text == t && maxW == w && size == s && col == c;
      }
    };

    // maxW <= 0 = untruncated. renderText clips rather than ellipsizes, so a wide
    // title is shortened here and re-rendered to fit.
    // A null window means one of the fixed slots, which are never swept.
    SP<Render::ITexture> windowLabel(const PHLWINDOW &win, const std::string &text,
                                     const CHyprColor &col, int size, int maxW = 0);
    SP<Render::ITexture> workspaceLabel(const PHLWORKSPACE &ws,
                                        const std::string &text,
                                        const CHyprColor &col, int size,
                                        int maxW = 0);
    // For a virtual workspace: numbered, no object, so it cannot be swept either.
    SP<Render::ITexture> virtualLabel(WORKSPACEID id, const std::string &text,
                                      const CHyprColor &col, int size,
                                      int maxW = 0);
    SP<Render::ITexture> fixedLabel(Slot slot, const std::string &text,
                                    const CHyprColor &col, int size,
                                    int maxW = 0);

    // The close glyph, shared by every card: glyph() renders, closeGlyph() reads.
    SP<Render::ITexture> glyph(const std::string &icon, const CHyprColor &col);
    [[nodiscard]] SP<Render::ITexture> closeGlyph() const;

    void putSnapshot(const PHLWINDOW &win, const SP<Render::ITexture> &tex);
    [[nodiscard]] SP<Render::ITexture> snapshot(const PHLWINDOW &win) const;
    void clearSnapshots() { m_snapshots.clear(); }

    void clear();

  private:
    template <typename Map, typename T>
    SP<Render::ITexture> labelIn(Map &map, const CWeakPointer<T> &owner,
                                 const std::string &text, const CHyprColor &col,
                                 int size, int maxW) {
      const auto key = owner.lock().get();
      if (!key)
        return nullptr;
      auto &slot = map[key];
      // The address is in use again, so the entry sitting there may belong to whoever
      // was here last. An expired reference is the only proof that it is stale --
      // the pointers compare equal, which is the entire problem.
      if (slot.owner.expired())
        slot = {};
      slot.owner = owner;
      auto &e = slot.entry;
      if (e.matches(text, maxW, size, col))
        return e.tex;
      e = LabelEntry{truncate(text, col, size, maxW), maxW, size, col, nullptr};
      e.tex = render(e.text, col, size);
      return e.tex;
    }

    [[nodiscard]] static SP<Render::ITexture> render(const std::string &s,
                                                     const CHyprColor &col, int size);
    // The longest prefix plus an ellipsis that fits maxW, or text when it fits.
    [[nodiscard]] static std::string truncate(const std::string &text,
                                              const CHyprColor &col, int size,
                                              int maxW);

    // Label entries that own what they describe. The weak reference is the entry's
    // ticket out: when it expires, the next lookup through the same address drops
    // the stale entry instead of handing it to a new occupant.
    struct WinLabel {
      PHLWINDOWREF owner;
      LabelEntry entry;
    };
    struct WsLabel {
      PHLWORKSPACEREF owner;
      LabelEntry entry;
    };
    struct WinSnapshot {
      PHLWINDOWREF owner;
      SP<Render::ITexture> tex;
    };

    std::unordered_map<Desktop::View::CWindow *, WinLabel> m_winLabels;
    std::unordered_map<CWorkspace *, WsLabel> m_wsLabels;
    // Never swept, but kept apart on purpose: a workspace id is an int64 and the
    // special ones are negative, so sharing a map with Slot would let a workspace
    // land on a slot's key.
    std::unordered_map<Slot, LabelEntry> m_slotLabels;
    std::unordered_map<WORKSPACEID, LabelEntry> m_virtualLabels;
    std::unordered_map<Desktop::View::CWindow *, WinSnapshot> m_snapshots;
    SP<Render::ITexture> m_glyph;
    std::string m_glyphIcon;
};

} // namespace gloview