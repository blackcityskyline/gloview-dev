#pragma once

#include <array>

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprutils/math/Vector2D.hpp>

namespace Render {
class IFramebuffer;
class ITexture;
}

namespace gloview {

// Self-contained gaussian blur for the overview backdrop, with its own GL
// program: Hyprland's blur reads GLOBAL decoration:blur:* through static
// values a plugin cannot override per call, and downscales damage by 1/2 per
// pass, which looks stepped full-screen.
//
// Dual-Kawase pyramid, not one jump: W×H halves down to W/2^levels (the
// configured `resolution`), runs `passes` H+V gaussians at the bottom, then
// halves back up. Stepping matters — a single bilinear sample only filters
// properly for a 2x reduction, so one jump to W/resolution aliases above 2.
// Every step is a real box/tent filter, and total pixel throughput stays under
// 2/3 of a full-res pass.
//
// `resolution` (pyramid depth) is the PRIMARY strength control; `size`/`passes`
// shape further via the gaussian stage. Needs an active monitor render with the
// projection at RPT_EXPORT; the filter owns viewport/fbSize for its FBOs.
class CBlurFilter {
  public:
    CBlurFilter()  = default;
    ~CBlurFilter();

    CBlurFilter(const CBlurFilter&)            = delete;
    CBlurFilter& operator=(const CBlurFilter&) = delete;

    // (Re)initialise the GL program if any parameter changed.
    void prepare(int passes, float sizePx, int resolution, float strength);

    // Blurs `src` (full-res monitor texture) into `dst` (full-res framebuffer,
    // e.g. the persistent cache). `dst` is fully overwritten with an OPAQUE
    // result — no dim, no tint, the caller draws colour on top. False when the
    // program/FBOs aren't ready: the caller falls back to an unblurred backdrop.
    bool render(const SP<Render::ITexture>& src, const SP<Render::IFramebuffer>& dst, int W, int H);

  private:
    bool compileProgram();
    void destroyProgram();
    void ensureScratch(int w, int h);
    void blitTex(const SP<Render::ITexture>& srcTex, GLuint prog, int locTex);
    void gaussPass(const SP<Render::ITexture>& srcTex, const SP<Render::IFramebuffer>& dstFB, const Vector2D& texSize, const Vector2D& dir, float radius);
    // `sizeForHalfpixel` is the DESTINATION size for downPass (0.5/dst) and the
    // SOURCE size for upPass (0.5/src) — the standard Kawase conventions.
    void downPass(const SP<Render::ITexture>& srcTex, const SP<Render::IFramebuffer>& dstFB, const Vector2D& dstSize);
    void upPass(const SP<Render::ITexture>& srcTex, const Vector2D& srcSize, const SP<Render::IFramebuffer>& dstFB);

    int   m_passes = 3;
    float m_sizePx = 8.F;
    int   m_resolution = 4;
    float m_strength = 1.F;

    int m_blurW = 0, m_blurH = 0; // bottom-of-pyramid size (post gaussian)
    SP<Render::IFramebuffer> m_fbA, m_fbB;

    // Levels 1..(m_levels-1); level m_levels IS m_fbA/m_fbB, level 0 is the
    // caller's src. Shared by both chains (a level's down-chain content is dead
    // by the time the up-chain writes it), so this is the pyramid's whole extra
    // cost over a single-buffer implementation.
    static constexpr int                                kMaxLevels = 5; // 2^5 == 32, matches the resolution clamp
    int                                                  m_levels = 0;
    std::array<SP<Render::IFramebuffer>, kMaxLevels - 1> m_downFBs;

    unsigned int m_program = 0, m_blitProg = 0, m_downProg = 0, m_upProg = 0; // GLuint
    int m_uTex = -1, m_uTexSize = -1, m_uRadius = -1, m_uDir = -1, m_uBlitTex = -1;
    int m_uDownTex = -1, m_uDownHalf = -1, m_uUpTex = -1, m_uUpHalf = -1;
    unsigned int m_vao = 0, m_vbo = 0; // GLuint
};

} // namespace gloview
