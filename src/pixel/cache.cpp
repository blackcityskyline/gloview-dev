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

SP<Render::ITexture> PixelCache::windowLabel(const PHLWINDOW &win,
                                            const std::string &text,
                                            const CHyprColor &col, int size,
                                            int maxW) {
  return labelIn(m_winLabels, PHLWINDOWREF{win}, text, col, size, maxW);
}

SP<Render::ITexture> PixelCache::workspaceLabel(const PHLWORKSPACE &ws,
                                                 const std::string &text,
                                                 const CHyprColor &col, int size,
                                                 int maxW) {
  return labelIn(m_wsLabels, PHLWORKSPACEREF{ws}, text, col, size, maxW);
}

SP<Render::ITexture> PixelCache::virtualLabel(WORKSPACEID id,
                                               const std::string &text,
                                               const CHyprColor &col, int size,
                                               int maxW) {
  auto &e = m_virtualLabels[id];
  // Hit first: nothing to measure. Probing before the lookup rendered the full
  // string every frame to discover that nothing had changed.
  if (e.matches(text, maxW, size, col))
    return e.tex;
  e = LabelEntry{truncate(text, col, size, maxW), maxW, size, col, nullptr};
  e.tex = render(e.text, col, size);
  return e.tex;
}

SP<Render::ITexture> PixelCache::fixedLabel(Slot slot, const std::string &text,
                                             const CHyprColor &col, int size,
                                             int maxW) {
  auto &e = m_slotLabels[slot];
  // Hit first: nothing to measure. Probing before the lookup rendered the full
  // string every frame to discover that nothing had changed.
  if (e.matches(text, maxW, size, col))
    return e.tex;
  e = LabelEntry{truncate(text, col, size, maxW), maxW, size, col, nullptr};
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

void PixelCache::putSnapshot(const PHLWINDOW &win,
                             const SP<Render::ITexture> &tex) {
  const PHLWINDOWREF ref = win;
  const auto         key = win.get();
  if (!key)
    return;
  auto &slot = m_snapshots[key];
  if (slot.owner.expired())
    slot = {}; // the address has a new tenant
  slot.owner = ref;
  slot.tex = tex;
}

SP<Render::ITexture> PixelCache::snapshot(const PHLWINDOW &win) const {
  const auto key = win.get();
  if (!key)
    return nullptr;
  const auto it = m_snapshots.find(key);
  // An expired owner means the window that put this texture here is gone; whatever
  // now lives at this address has nothing to do with it.
  if (it == m_snapshots.end() || it->second.owner.expired())
    return nullptr;
  return it->second.tex;
}

void PixelCache::clear() {
  m_winLabels.clear();
  m_wsLabels.clear();
  m_slotLabels.clear();
  m_virtualLabels.clear();
  m_snapshots.clear();
  m_glyph.reset();
  m_glyphIcon.clear();
}

} // namespace gloview