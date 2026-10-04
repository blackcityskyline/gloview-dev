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

// needs a live monitor render at RPT_EXPORT
class CBlurFilter {
  public:
    CBlurFilter()  = default;
    ~CBlurFilter();

    CBlurFilter(const CBlurFilter&)            = delete;
    CBlurFilter& operator=(const CBlurFilter&) = delete;

    void prepare(int passes, float sizePx, int resolution, float strength);

    // opaque, no dim/tint; false = program/FBOs not ready
    bool render(const SP<Render::ITexture>& src, const SP<Render::IFramebuffer>& dst, int W, int H);

  private:
    bool compileProgram();
    void destroyProgram();
    void ensureScratch(int w, int h);
    void blitTex(const SP<Render::ITexture>& srcTex, GLuint prog, int locTex);
    void gaussPass(const SP<Render::ITexture>& srcTex, const SP<Render::IFramebuffer>& dstFB, const Vector2D& texSize, const Vector2D& dir, float radius);
    void downPass(const SP<Render::ITexture>& srcTex, const SP<Render::IFramebuffer>& dstFB, const Vector2D& dstSize);
    void upPass(const SP<Render::ITexture>& srcTex, const Vector2D& srcSize, const SP<Render::IFramebuffer>& dstFB);

    int   m_passes = 3;
    float m_sizePx = 8.F;
    int   m_resolution = 4;
    float m_strength = 1.F;

    int m_blurW = 0, m_blurH = 0;
    SP<Render::IFramebuffer> m_fbA, m_fbB;

    // shared by both chains: down content is dead at the up-chain
    static constexpr int                                kMaxLevels = 5;
    int                                                  m_levels = 0;
    std::array<SP<Render::IFramebuffer>, kMaxLevels - 1> m_downFBs;

    unsigned int m_program = 0, m_blitProg = 0, m_downProg = 0, m_upProg = 0;
    int m_uTex = -1, m_uTexSize = -1, m_uRadius = -1, m_uDir = -1, m_uBlitTex = -1;
    int m_uDownTex = -1, m_uDownHalf = -1, m_uUpTex = -1, m_uUpHalf = -1;
    unsigned int m_vao = 0, m_vbo = 0;
};

}
