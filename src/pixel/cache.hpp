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

// Captions, the close glyph, entry snapshots. The job is the KEYING POLICY: a
// caption is asked for whole and cut to the card, and sharing a slot between the
// two measures one texture and draws the other. Keys are window addresses, safe
// only because dead ones are pruned — Hyprland reuses addresses.
class PixelCache {
  public:
    // maxW <= 0 = untruncated. renderText clips rather than ellipsizes, so a title
    // too wide for its card is shortened and re-rendered to fit.
    SP<Render::ITexture> label(void *key, const std::string &text,
                               const CHyprColor &col, int size,
                               int maxW = 0);

    // Reads without rendering; paint must not create entries.
    [[nodiscard]] SP<Render::ITexture> peekLabel(void *key, int maxW) const;

    // The close glyph, shared by every card that shows one. glyph() renders,
    // closeGlyph() reads.
    SP<Render::ITexture> glyph(const std::string &icon, const CHyprColor &col);
    [[nodiscard]] SP<Render::ITexture> closeGlyph() const;

    void putSnapshot(void *win, const SP<Render::ITexture> &tex);
    [[nodiscard]] SP<Render::ITexture> snapshot(void *win) const;
    [[nodiscard]] const std::unordered_map<void *, SP<Render::ITexture>> &
    snapshots() const {
      return m_snapshots;
    }
    void clearSnapshots() { m_snapshots.clear(); }

    // Drops everything; the close path, where the keys' windows are going away.
    void clear();

    // Keys whose window is not in `live`.
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