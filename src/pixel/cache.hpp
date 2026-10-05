#pragma once

#include <unordered_map>
#include <string>
#include <vector>

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace Render {
class ITexture;
class IFramebuffer;
} // namespace Render

namespace gloview::pixel {

// Every texture the plugin renders more than once per frame or reuses across
// frames: captions, the close glyph, entry snapshots.
//
// The module's job is the KEYING POLICY, not holding the textures. A caption is
// asked for at two widths — whole, when the card width is not yet known, and cut
// to the card — and those must not share a cache slot, or the pill measures one
// texture and draws the other.
//
// Keys are window addresses because that is what the callers hold. Safe only
// because dead keys are pruned: an address is reused by Hyprland, and a stale
// entry under a live address would serve the previous window's caption forever.
class PixelCache {
  public:
    // maxW <= 0 means untruncated. Rendered and stored on a miss.
    // renderText clips rather than ellipsizes, so a title too wide for its card is
    // shortened here by binary search on the character count and re-rendered at
    // the size that fits.
    SP<Render::ITexture> label(void *key, const std::string &text,
                               const CHyprColor &col, int size,
                               int maxW = 0);

    // Reads without rendering: nullptr when nothing was captured for this
    // key/width. Paint must not create entries, or it stops being a pure
    // function of (Model, Clock, Pixel).
    [[nodiscard]] SP<Render::ITexture> peekLabel(void *key, int maxW) const;

    // The close glyph, shared by every card that shows one. Same capture/read
    // split as labels: glyph() renders, closeGlyph() reads.
    SP<Render::ITexture> glyph(const std::string &icon, const CHyprColor &col);
    [[nodiscard]] SP<Render::ITexture> closeGlyph() const;

    void putSnapshot(void *win, const SP<Render::ITexture> &tex);
    [[nodiscard]] SP<Render::ITexture> snapshot(void *win) const;
    [[nodiscard]] const std::unordered_map<void *, SP<Render::ITexture>> &
    snapshots() const {
      return m_snapshots;
    }
    void clearSnapshots() { m_snapshots.clear(); }

    // Drops everything. Called on the close path, where the windows the keys
    // point at are going away anyway.
    void clear();

    // Keys whose window is not in `live`. Without this the caption cache grows
    // for the lifetime of the compositor and holds a texture per closed window.
    void prune(const std::vector<void *> &live);

  private:
    struct LabelEntry {
      std::string text;
      int maxW = 0;
      SP<Render::ITexture> tex;
    };

    [[nodiscard]] static SP<Render::ITexture> render(const std::string &s,
                                                    const CHyprColor &col,
                                                    int size);
    // The longest prefix plus an ellipsis that fits maxW, or text when it fits.
    [[nodiscard]] static std::string truncate(const std::string &text,
                                              const CHyprColor &col, int size,
                                              int maxW);

    std::unordered_map<void *, LabelEntry> m_labels;
    std::unordered_map<void *, SP<Render::ITexture>> m_snapshots;
    SP<Render::ITexture> m_glyph;
    std::string m_glyphIcon;
};

} // namespace gloview