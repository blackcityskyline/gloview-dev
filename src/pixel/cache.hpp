#pragma once

#include <cstdint>

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

// Cache entries that have no window behind them still need a stable key, and a
// string-literal address would be one -- but at a call site it reads as a window
// address. Negative integers, so they can never collide with a real pointer, and
// spelled as integers because a constexpr void* is not a constant expression.
inline constexpr std::intptr_t kAllCardsKey = -2;     // the strip's "All" card
inline constexpr std::intptr_t kCarouselCountKey = -1; // the carousel's counter pill

// Keys above are integers so they can be constexpr; the cache is keyed by void*.
inline void *syntheticKey(std::intptr_t k) {
  return reinterpret_cast<void *>(k);
}

// Captions, the close glyph, entry snapshots. The job is the KEYING POLICY: a
// caption is asked for whole and cut to the card, and sharing a slot measures one
// texture and draws the other. Keys are window addresses, safe only because dead
// ones are pruned — Hyprland reuses addresses, so a closed window's entry would
// hand its snapshot to whatever window lands on that address next.
class PixelCache {
  public:
    // maxW <= 0 = untruncated. renderText clips rather than ellipsizes, so a wide
    // title is shortened here and re-rendered to fit.
    SP<Render::ITexture> label(void *key, const std::string &text,
                               const CHyprColor &col, int size,
                               int maxW = 0);

    // The close glyph, shared by every card: glyph() renders, closeGlyph() reads.
    SP<Render::ITexture> glyph(const std::string &icon, const CHyprColor &col);
    [[nodiscard]] SP<Render::ITexture> closeGlyph() const;

    void putSnapshot(void *win, const SP<Render::ITexture> &tex);
    [[nodiscard]] SP<Render::ITexture> snapshot(void *win) const;
    [[nodiscard]] const std::unordered_map<void *, SP<Render::ITexture>> &
    snapshots() const {
      return m_snapshots;
    }
    void clearSnapshots() { m_snapshots.clear(); }

    void clear();


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