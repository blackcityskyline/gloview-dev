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

// Self-contained, configurable gaussian blur for the overview backdrop.
//
// Hyprland's own blur (renderRect({.blur}) / blurMainFramebuffer) reads the
// GLOBAL decoration:blur:* settings through static CConfigValues that a plugin
// cannot override per-call, and its pipeline progressively downscales the
// damage by 1/2 per pass (3 passes -> ~1/8 res), which looks "stepped" when
// applied full-screen. This filter re-implements the blur with its own GL
// program so passes / size / quality are tunable keys.
//
// Scaling is done as a dual-Kawase pyramid, not one single jump:
//
//   source (W x H)
//     --down-->  L1 (W/2   x H/2)
//     --down-->  L2 (W/4   x H/4)
//     ...
//     --down-->  A  (W/2^levels x H/2^levels)   [== the configured `resolution`]
//   for p in 1..passes: A --H gauss--> B ; B --V gauss--> A
//     --up-->    ...
//     --up-->    L1
//     --up-->    dst (W x H, opaque)
//
// A single non-mipmapped bilinear sample only ever properly filters a 2x
// reduction; jumping straight from full res to W/resolution in one sample (the
// previous implementation) aliases for any resolution > 2, which is what
// produced a blocky/pixelated look — especially visible at higher output
// resolutions (1440p/4K) where `resolution` is naturally larger to keep the
// gaussian stage's buffer a sane size. Stepping down (and back up) in strict
// halvings keeps every single sample a proper 2x box/tent filter, so the
// result stays smooth at any output size, and each step's cost is
// proportional to ITS OWN (shrinking) resolution — the down+up chain's total
// pixel throughput sums to a bit under 2/3 of one full-res pass (geometric
// series, dominated by the first halving), so this is not "blur at full res
// instead" in disguise: the only full-resolution-sized touches are the very
// first down-sample and the very last up-sample, exactly as many as the old
// single-jump version had.
//
// `resolution` keeps its existing meaning (how far down the gaussian stage's
// buffer is from full res) — it's now realised as round(log2(resolution))
// pyramid levels, i.e. the nearest power of two, clamped to kMaxLevels (32x).
// `passes` / `sizePx` / `strength` keep controlling the gaussian stage
// exactly as before. Linear filtering at every stage keeps transitions smooth.
//
// `resolution` (pyramid depth) is the PRIMARY blur-strength control: each
// Kawase down+up step is a real box/tent filter over its input, so more
// levels compounds into substantially more blur on its own — this is the
// whole basis of dual-Kawase blur (KDE/GNOME use it as their entire blur,
// no separate gaussian stage at all). `size`/`passes` add further shaping on
// top via the gaussian stage at the bottom of the pyramid.
//
// Must be called with an active monitor render (m_renderData.pMonitor set) and
// the projection set to RPT_EXPORT; the filter switches viewport/fbSize itself
// for the intermediate FBOs and leaves the final viewport at (W,H).
class CBlurFilter {
  public:
    CBlurFilter()  = default;
    ~CBlurFilter();

    CBlurFilter(const CBlurFilter&)            = delete;
    CBlurFilter& operator=(const CBlurFilter&) = delete;

    // (Re)initialise the GL program if any parameter changed.
    void prepare(int passes, float sizePx, int resolution, float strength);

    // Blur `src` (the full-res monitor texture) into `dst` (a full-res
    // framebuffer, e.g. the persistent blur cache) and composite `dim` on top.
    // `dst` is fully overwritten with an opaque result. Returns false if the
    // GL program/FBOs aren't ready — the caller should then fall back to an
    // unblurred backdrop.
    bool render(const SP<Render::ITexture>& src, const SP<Render::IFramebuffer>& dst, int W, int H, const CHyprColor& dim);

  private:
    bool compileProgram();
    void destroyProgram();
    void ensureScratch(int w, int h);
    void blitTex(const SP<Render::ITexture>& srcTex, GLuint prog, int locTex);
    void gaussPass(const SP<Render::ITexture>& srcTex, const SP<Render::IFramebuffer>& dstFB, const Vector2D& texSize, const Vector2D& dir, float radius);
    // Dual-Kawase pyramid steps. `sizeForHalfpixel` is the DESTINATION size for
    // downPass (halfpixel = 0.5/dst, the standard Kawase-down convention) and the
    // SOURCE size for upPass (halfpixel = 0.5/src, the standard Kawase-up convention).
    void downPass(const SP<Render::ITexture>& srcTex, const SP<Render::IFramebuffer>& dstFB, const Vector2D& dstSize);
    void upPass(const SP<Render::ITexture>& srcTex, const Vector2D& srcSize, const SP<Render::IFramebuffer>& dstFB);

    int   m_passes = 3;
    float m_sizePx = 8.F;
    int   m_resolution = 4;
    float m_strength = 1.F;

    int m_blurW = 0, m_blurH = 0; // bottom-of-pyramid buffer size (post gaussian stage)
    SP<Render::IFramebuffer> m_fbA, m_fbB;

    // Intermediate pyramid levels 1..(m_levels-1); level m_levels IS m_fbA/m_fbB
    // above, level 0 is the caller's own `src` texture (never copied into an FBO of
    // ours). Reused for BOTH the down-chain and the up-chain (a level's down-chain
    // content is no longer needed by the time the up-chain writes into it), so this
    // is the total extra scratch memory the pyramid costs beyond the previous
    // single-buffer implementation.
    static constexpr int                                kMaxLevels = 5; // 2^5 == 32, matches the resolution config's clamp
    int                                                  m_levels = 0;
    std::array<SP<Render::IFramebuffer>, kMaxLevels - 1> m_downFBs;

    unsigned int m_program = 0, m_blitProg = 0, m_colProg = 0, m_downProg = 0, m_upProg = 0; // GLuint
    int m_uTex = -1, m_uTexSize = -1, m_uRadius = -1, m_uDir = -1, m_uBlitTex = -1, m_uCol = -1;
    int m_uDownTex = -1, m_uDownHalf = -1, m_uUpTex = -1, m_uUpHalf = -1;
    unsigned int m_vao = 0, m_vbo = 0; // GLuint
};

} // namespace gloview
