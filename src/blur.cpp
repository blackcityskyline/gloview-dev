#include "blur.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <GLES3/gl32.h>

#include <algorithm>
#include <cmath>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

const char* const VERT_SRC = R"#(
#version 300 es
precision highp float;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    vUv  = aUv;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)#";

// 9-tap separable gaussian at the pyramid BOTTOM: cost ignores output resolution.
const char* const FRAG_SRC = R"#(
#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec2      uTexSize; // source texture size in pixels
uniform vec2      uDir;     // (1,0) horizontal or (0,1) vertical
uniform float     uRadius;  // blur radius in source texels
in vec2 vUv;
out vec4 fragColor;

void main() {
    vec2  stepPx  = uDir / uTexSize;
    float r       = max(uRadius, 0.5);
    float spacing = r / 4.0;
    // exp(-i^2 / 18) for i in -4..4, normalized (sum = 6.52868)
    const float invSum = 1.0 / 6.52868;
    vec4  sum = vec4(0.0);
    for (int i = -4; i <= 4; ++i) {
        float w  = exp(-float(i * i) / 18.0) * invSum;
        vec2  off = stepPx * spacing * float(i);
        sum += texture(uTex, vUv + off) * w;
    }
    // Force opaque: a semi-transparent blur layer would let the sharp desktop
    // show through on the final screen blit (double-image on transparent bars).
    fragColor = vec4(sum.rgb, 1.0);
}
)#";

const char* const FRAG_BLIT = R"#(
#version 300 es
precision highp float;
uniform sampler2D uTex;
in vec2 vUv;
out vec4 fragColor;
void main() {
    fragColor = vec4(texture(uTex, vUv).rgb, 1.0);
}
)#";

// 5-tap diamond downsample; uHalfpixel is 0.5 / DESTINATION size, not source.
const char* const FRAG_KAWASE_DOWN = R"#(
#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec2      uHalfpixel;
in vec2 vUv;
out vec4 fragColor;
void main() {
    vec4 sum = texture(uTex, vUv) * 4.0;
    sum += texture(uTex, vUv - uHalfpixel);
    sum += texture(uTex, vUv + uHalfpixel);
    sum += texture(uTex, vUv + vec2(uHalfpixel.x, -uHalfpixel.y));
    sum += texture(uTex, vUv - vec2(uHalfpixel.x, -uHalfpixel.y));
    fragColor = vec4((sum / 8.0).rgb, 1.0);
}
)#";

// Matching 8-tap upsample; here uHalfpixel is 0.5 / SOURCE size.
const char* const FRAG_KAWASE_UP = R"#(
#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec2      uHalfpixel;
in vec2 vUv;
out vec4 fragColor;
void main() {
    vec4 sum = texture(uTex, vUv + vec2(-uHalfpixel.x * 2.0, 0.0));
    sum += texture(uTex, vUv + vec2(-uHalfpixel.x, uHalfpixel.y)) * 2.0;
    sum += texture(uTex, vUv + vec2(0.0, uHalfpixel.y * 2.0));
    sum += texture(uTex, vUv + vec2(uHalfpixel.x, uHalfpixel.y)) * 2.0;
    sum += texture(uTex, vUv + vec2(uHalfpixel.x * 2.0, 0.0));
    sum += texture(uTex, vUv + vec2(uHalfpixel.x, -uHalfpixel.y)) * 2.0;
    sum += texture(uTex, vUv + vec2(0.0, -uHalfpixel.y * 2.0));
    sum += texture(uTex, vUv + vec2(-uHalfpixel.x, -uHalfpixel.y)) * 2.0;
    fragColor = vec4((sum / 12.0).rgb, 1.0);
}
)#";

GLuint compileShader(GLenum type, const char* src) {
    const GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        Log::logger->log(Log::ERR, "gloview blur: shader compile failed: {}", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint linkProgram(const char* vsSrc, const char* fsSrc) {
    const GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs)
        return 0;

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        Log::logger->log(Log::ERR, "gloview blur: program link failed: {}", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

}


CBlurFilter::~CBlurFilter() {
    destroyProgram();
}

void CBlurFilter::prepare(int passes, float sizePx, int resolution, float strength) {
    passes     = std::clamp(passes, 1, 16);
    sizePx     = std::clamp(sizePx, 1.0F, 200.0F);
    resolution = std::clamp(resolution, 1, 32);
    strength   = std::clamp(strength, 0.01F, 4.0F);

    m_passes = passes;
    m_sizePx = sizePx;
    m_resolution = resolution;
    m_strength = strength;

    if (!m_program)
        compileProgram();
}

bool CBlurFilter::compileProgram() {
    destroyProgram();

    m_program  = linkProgram(VERT_SRC, FRAG_SRC);
    m_blitProg = linkProgram(VERT_SRC, FRAG_BLIT);
    m_downProg = linkProgram(VERT_SRC, FRAG_KAWASE_DOWN);
    m_upProg   = linkProgram(VERT_SRC, FRAG_KAWASE_UP);
    if (!m_program || !m_blitProg || !m_downProg || !m_upProg)
        return false;

    m_uTex      = glGetUniformLocation(m_program, "uTex");
    m_uTexSize  = glGetUniformLocation(m_program, "uTexSize");
    m_uRadius   = glGetUniformLocation(m_program, "uRadius");
    m_uDir      = glGetUniformLocation(m_program, "uDir");
    m_uBlitTex  = glGetUniformLocation(m_blitProg, "uTex");
    m_uDownTex  = glGetUniformLocation(m_downProg, "uTex");
    m_uDownHalf = glGetUniformLocation(m_downProg, "uHalfpixel");
    m_uUpTex    = glGetUniformLocation(m_upProg, "uTex");
    m_uUpHalf   = glGetUniformLocation(m_upProg, "uHalfpixel");

    if (m_vao == 0)
        glGenVertexArrays(1, &m_vao);
    if (m_vbo == 0)
        glGenBuffers(1, &m_vbo);

    const float verts[4][4] = {
        {-1.0F, -1.0F, 0.0F, 0.0F},
        { 1.0F, -1.0F, 1.0F, 0.0F},
        {-1.0F,  1.0F, 0.0F, 1.0F},
        { 1.0F,  1.0F, 1.0F, 1.0F},
    };

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void CBlurFilter::destroyProgram() {
    if (m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
    if (m_blitProg) {
        glDeleteProgram(m_blitProg);
        m_blitProg = 0;
    }
    if (m_downProg) {
        glDeleteProgram(m_downProg);
        m_downProg = 0;
    }
    if (m_upProg) {
        glDeleteProgram(m_upProg);
        m_upProg = 0;
    }
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }
}

void CBlurFilter::ensureScratch(int w, int h) {
    // Dyadic halvings down to ~resolution as a power of two: every step must be
    // a clean 2x. Clamped to kMaxLevels for the fixed-size FB array.
    const int levels = std::clamp(static_cast<int>(std::lround(std::log2(static_cast<double>(std::max(1, m_resolution))))), 0, kMaxLevels);

    // Same halving walk as the allocation: a plain W/resolution divide disagrees.
    int bw = w, bh = h;
    for (int i = 0; i < levels; ++i) {
        bw = std::max(1, (bw + 1) / 2);
        bh = std::max(1, (bh + 1) / 2);
    }

    const bool bottomChanged = (m_blurW != bw || m_blurH != bh || !m_fbA || !m_fbB);
    const bool levelsChanged = (m_levels != levels);
    if (!bottomChanged && !levelsChanged)
        return;

    m_levels = levels;
    m_blurW  = bw;
    m_blurH  = bh;

    if (!m_fbA)
        m_fbA = g_pHyprRenderer->createFB("gloview blur A");
    if (!m_fbB)
        m_fbB = g_pHyprRenderer->createFB("gloview blur B");
    if (!m_fbA->isAllocated() || m_fbA->m_size != Vector2D(bw, bh))
        m_fbA->alloc(bw, bh);
    if (!m_fbB->isAllocated() || m_fbB->m_size != Vector2D(bw, bh))
        m_fbB->alloc(bw, bh);

    // Level 0 is the caller's source texture, never copied into an FBO of ours.
    int lw = w, lh = h;
    for (int i = 0; i < m_levels - 1; ++i) {
        lw = std::max(1, (lw + 1) / 2);
        lh = std::max(1, (lh + 1) / 2);
        if (!m_downFBs[i])
            m_downFBs[i] = g_pHyprRenderer->createFB("gloview blur pyramid");
        if (!m_downFBs[i]->isAllocated() || m_downFBs[i]->m_size != Vector2D(lw, lh))
            m_downFBs[i]->alloc(lw, lh);
    }
}

void CBlurFilter::blitTex(const SP<Render::ITexture>& srcTex, GLuint prog, int locTex) {
    glDisable(GL_BLEND);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    srcTex->bind();
    glUniform1i(locTex, 0);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    srcTex->unbind();
    glUseProgram(0);
}

void CBlurFilter::gaussPass(const SP<Render::ITexture>& srcTex, const SP<Render::IFramebuffer>& dstFB, const Vector2D& texSize, const Vector2D& dir, float radius) {
    dstFB->bind();
    g_pHyprOpenGL->setViewport(0, 0, (int)dstFB->m_size.x, (int)dstFB->m_size.y);
    glDisable(GL_BLEND);
    glUseProgram(m_program);
    glActiveTexture(GL_TEXTURE0);
    srcTex->bind();
    glUniform1i(m_uTex, 0);
    glUniform2f(m_uTexSize, texSize.x, texSize.y);
    glUniform2f(m_uDir, dir.x, dir.y);
    glUniform1f(m_uRadius, radius);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    srcTex->unbind();
    glUseProgram(0);
}

void CBlurFilter::downPass(const SP<Render::ITexture>& srcTex, const SP<Render::IFramebuffer>& dstFB, const Vector2D& dstSize) {
    dstFB->bind();
    g_pHyprOpenGL->setViewport(0, 0, (int)dstFB->m_size.x, (int)dstFB->m_size.y);
    glDisable(GL_BLEND);
    glUseProgram(m_downProg);
    glActiveTexture(GL_TEXTURE0);
    srcTex->bind();
    glUniform1i(m_uDownTex, 0);
    glUniform2f(m_uDownHalf, 0.5F / std::max(1.0F, static_cast<float>(dstSize.x)), 0.5F / std::max(1.0F, static_cast<float>(dstSize.y)));
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    srcTex->unbind();
    glUseProgram(0);

}

void CBlurFilter::upPass(const SP<Render::ITexture>& srcTex, const Vector2D& srcSize, const SP<Render::IFramebuffer>& dstFB) {
    dstFB->bind();
    g_pHyprOpenGL->setViewport(0, 0, (int)dstFB->m_size.x, (int)dstFB->m_size.y);
    glDisable(GL_BLEND);
    glUseProgram(m_upProg);
    glActiveTexture(GL_TEXTURE0);
    srcTex->bind();
    glUniform1i(m_uUpTex, 0);
    glUniform2f(m_uUpHalf, 0.5F / std::max(1.0F, static_cast<float>(srcSize.x)), 0.5F / std::max(1.0F, static_cast<float>(srcSize.y)));
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    srcTex->unbind();
    glUseProgram(0);

}

bool CBlurFilter::render(const SP<Render::ITexture>& src, const SP<Render::IFramebuffer>& dst, int W, int H) {
    if (!src || !src->ok() || !dst || !dst->isAllocated() || !m_program || !m_blitProg || !m_downProg || !m_upProg)
        return false;

    ensureScratch(W, H);

    // Snapshot the caller's blend FUNC, not just the enable bit: passes disable blending.
    const bool blendBefore = glIsEnabled(GL_BLEND) == GL_TRUE;
    GLint      blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    // Scissor and caller FBO/viewport too: a stray scissor clips the quads.
    const bool scissorBefore = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glDisable(GL_SCISSOR_TEST);
    const auto callerFB = g_pHyprRenderer->m_renderData.currentFB;
    GLint      callerVp[4];
    glGetIntegerv(GL_VIEWPORT, callerVp);

    // Radius is in the bottom buffer's texels (1 << m_levels), not raw config.
    const float    strengthRadius = std::max(0.5F, (m_sizePx * m_strength) / static_cast<float>(1 << m_levels));
    const Vector2D blurSize{static_cast<float>(m_blurW), static_cast<float>(m_blurH)};

    // Down-chain: each step is a real 2x filter, never one aliasing jump.
    if (m_levels == 0) {
        m_fbA->bind();
        g_pHyprOpenGL->setViewport(0, 0, m_blurW, m_blurH);
        blitTex(src, m_blitProg, m_uBlitTex);
    } else {
        SP<Render::ITexture> stepSrc = src;
        int                   lw = W, lh = H;
        for (int lvl = 1; lvl <= m_levels; ++lvl) {
            lw = std::max(1, (lw + 1) / 2);
            lh = std::max(1, (lh + 1) / 2);
            const auto& dstFB = (lvl == m_levels) ? m_fbA : m_downFBs[lvl - 1];
            downPass(stepSrc, dstFB, Vector2D(lw, lh));
            stepSrc = dstFB->getTexture();
        }
    }

    for (int p = 0; p < m_passes; ++p) {
        gaussPass(m_fbA->getTexture(), m_fbB, blurSize, {1.0F, 0.0F}, strengthRadius); // H: A->B
        gaussPass(m_fbB->getTexture(), m_fbA, blurSize, {0.0F, 1.0F}, strengthRadius); // V: B->A
    }

    // Up-chain reuses the down-chain FBOs: consume a level before overwriting it.
    if (m_levels == 0) {
        dst->bind();
        g_pHyprOpenGL->setViewport(0, 0, W, H);
        blitTex(m_fbA->getTexture(), m_blitProg, m_uBlitTex);
    } else {
        SP<Render::ITexture> stepSrc  = m_fbA->getTexture();
        Vector2D               srcSize = blurSize;
        for (int lvl = m_levels; lvl >= 1; --lvl) {
            const bool  toFullRes = (lvl == 1);
            const auto& dstFB     = toFullRes ? dst : m_downFBs[lvl - 2];
            upPass(stepSrc, srcSize, dstFB);
            if (!toFullRes) {
                stepSrc = dstFB->getTexture();
                srcSize = dstFB->m_size;
            }
        }
    }

    // No dim here: the cache stays free of backdrop_color and fade; renderBackdrop() draws it.
    glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
    if (!blendBefore)
        glDisable(GL_BLEND);
    if (scissorBefore)
        glEnable(GL_SCISSOR_TEST);
    if (callerFB) {
        callerFB->bind();
        g_pHyprOpenGL->setViewport(callerVp[0], callerVp[1], callerVp[2], callerVp[3]);
    }

    return true;
}

}
