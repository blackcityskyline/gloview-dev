#include "cache.hpp"

#include <algorithm>

#include <hyprland/src/render/Renderer.hpp>

namespace gloview::pixel {

namespace {

// U+2026, added by hand because renderText clips instead of ellipsizing.
constexpr char kEllipsis[] = "\xE2\x80\xA6";

} // namespace

SP<Render::ITexture> PixelCache::render(const std::string &s,
                                        const CHyprColor &col, int size) {
  if (!g_pHyprRenderer)
    return nullptr;
  return g_pHyprRenderer->renderText(s, col, size, false, "", 0,
                                     size >= 15 ? 700 : 600);
}

std::string PixelCache::truncate(const std::string &text,
                                 const CHyprColor &col, int size, int maxW) {
  if (maxW <= 0 || text.size() <= 4)
    return text;
  const auto probe = render(text, col, size);
  if (!probe || probe->m_size.x <= maxW)
    return text;
  // Binary search on the character count, not the width: rendering a candidate
  // costs far more than the comparison.
  int lo = 1, hi = static_cast<int>(text.size()) - 1, best = 0;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const auto t =
        render(text.substr(0, static_cast<size_t>(mid)) + kEllipsis, col, size);
    if (t && t->m_size.x <= maxW) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  if (best <= 0)
    return text;
  return text.substr(0, static_cast<size_t>(best)) + kEllipsis;
}

SP<Render::ITexture> PixelCache::label(void *key, const std::string &text,
                                       const CHyprColor &col, int size,
                                       int maxW) {
  auto &e = m_labels[key];
  // Hit first: nothing to measure. Probing before the lookup rendered the full
  // string every frame to discover that nothing had changed.
  if (e.tex && e.text == text && e.maxW == maxW)
    return e.tex;
  e = LabelEntry{truncate(text, col, size, maxW), maxW, nullptr};
  e.tex = render(e.text, col, size);
  return e.tex;
}

SP<Render::ITexture> PixelCache::glyph(const std::string &icon,
                                        const CHyprColor &col) {
  if (!m_glyph || m_glyphIcon != icon) {
    m_glyph = g_pHyprRenderer
                  ? g_pHyprRenderer->renderText(icon, col, 16, false, "", 0, 800)
                  : nullptr;
    m_glyphIcon = icon;
  }
  return m_glyph;
}

SP<Render::ITexture> PixelCache::closeGlyph() const { return m_glyph; }

void PixelCache::putSnapshot(void *win, const SP<Render::ITexture> &tex) {
  m_snapshots[win] = tex;
}

SP<Render::ITexture> PixelCache::snapshot(void *win) const {
  const auto it = m_snapshots.find(win);
  return it == m_snapshots.end() ? nullptr : it->second;
}

void PixelCache::clear() {
  m_labels.clear();
  m_snapshots.clear();
  m_glyph.reset();
  m_glyphIcon.clear();
}

void PixelCache::prune(const std::vector<void *> &live) {
  const auto gone = [&](void *k) {
    return std::find(live.begin(), live.end(), k) == live.end();
  };
  for (auto it = m_labels.begin(); it != m_labels.end();) {
    it = gone(it->first) ? m_labels.erase(it) : std::next(it);
  }
  for (auto it = m_snapshots.begin(); it != m_snapshots.end();) {
    it = gone(it->first) ? m_snapshots.erase(it) : std::next(it);
  }
}

} // namespace gloview