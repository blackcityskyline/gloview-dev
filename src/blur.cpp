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

// Separable 9-tap gaussian. Tap spacing = uRadius/4 so the kernel footprint is
// ~+/-uRadius source texels; sigma = 3 in tap space. Sampling is linear, so a
// sub-texel radius degrades into a smooth 1-2 tap blur instead of aliasing.
// Runs at the BOTTOM of the dual-Kawase pyramid (see FRAG_KAWASE_DOWN/UP
// below), i.e. on the smallest buffer, so its 9 taps per axis stay cheap
// regardless of output resolution.
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

// Plain linear copy, used only for the degenerate resolution=1 case (no
// pyramid levels at all — see CBlurFilter::render()).
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

// Dual-Kawase downsample: a 5-tap diamond (center weight 4, four diagonal
// taps weight 1 each; sum 8) — the standard companion filter for an exact 2x
// reduction, which is the only ratio a single non-mipmapped bilinear sample
// can filter without aliasing. uHalfpixel = 0.5 / DESTINATION size, in
// normalised UV units (both textures share the same [0,1] UV rect, so the
// same vUv feeds source and destination).
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

// Dual-Kawase upsample: the matching 8-tap filter (4 corner taps weight 1, 4
// edge taps weight 2; sum 12) for a 2x enlargement. uHalfpixel = 0.5 / SOURCE
// size (the smaller texture being sampled FROM).
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

} // namespace

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

    // Fullscreen quad: position + uv, interleaved (stride 16 bytes).
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
    // How many dyadic (halving) pyramid steps get us from full res down to
    // (approximately) the configured `resolution` divisor — e.g. resolution=4
    // -> 2 levels (full -> 1/2 -> 1/4), resolution=6 -> 3 levels (-> 1/8, the
    // nearest power of two, since a fractional halving isn't meaningful and
    // every step must be a clean 2x for the Kawase filters to be correct).
    // Clamped to kMaxLevels (5, i.e. resolution up to 32) to bound the
    // fixed-size m_downFBs array.
    //
    // `resolution` (pyramid depth) is the PRIMARY blur-strength control, not
    // just an anti-aliasing detail: each Kawase down+up step is itself a real
    // box/tent filter over its input, and repeating it `levels` times is a
    // substantial, compounding blur on its own — this is the entire basis of
    // dual-Kawase blur as used by KDE/GNOME, who don't run a separate
    // gaussian pass at all. (An earlier version of this function additionally
    // capped `levels` down whenever the gaussian stage's OWN radius looked
    // too small in texel terms, on the assumption that the pyramid itself
    // contributed negligible blur — that assumption was wrong: it directly
    // reduced pyramid depth, and with it total blur strength, and made
    // `resolution` stop having any effect past that cap. Removed.)
    const int levels = std::clamp(static_cast<int>(std::lround(std::log2(static_cast<double>(std::max(1, m_resolution))))), 0, kMaxLevels);

    // Walk `levels` halvings to get the bottom-of-pyramid (post-gaussian) size —
    // same loop shape as the pyramid allocation below, so this is guaranteed to
    // land on exactly the same size the down-chain's last step actually produces
    // (unlike computing it independently via a plain W/resolution divide, which
    // can disagree with the halving chain for a non-power-of-two `resolution`).
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

    // Intermediate pyramid levels 1 .. levels-1 (level `levels` is m_fbA/m_fbB
    // above, level 0 is the caller's own source texture — never copied into an
    // FBO of ours). Reused for both the down-chain and the up-chain in render().
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

// The three pass helpers below are RAW: render() captures scissor/blend/FBO/
// viewport state once for the whole chain and restores it at the end — a
// save/restore scope per pass cost two viewport queries and two FBO rebinds
// per pass (~20 extra GL calls per re-blur, which is EVERY frame for a live
// video source).
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

    // Fully snapshot the caller's blend state up front — both whether it's enabled AND the
    // exact blend factors — so this call is a true no-op on it, regardless of which
    // convention (straight vs premultiplied alpha, or anything else) the surrounding
    // renderer happens to be using at the call site. The passes below all
    // unconditionally glDisable(GL_BLEND) for their own opaque draws — restoring
    // only the enable bit (an earlier version of this fix) put the right
    // enabled/disabled flag back but left OUR OWN blend func in place regardless of what the
    // caller actually had, which is wrong for anything relying on a different func.
    // Capturing the full state here and restoring all of it at the end removes that
    // guesswork entirely: whatever renderBackdrop()'s caller had set stays exactly as it
    // was, whether that's this straight-alpha func or something else downstream (e.g.
    // renderStrip(), drawn right after renderBackdrop() in the same pass) depends on.
    const bool blendBefore = glIsEnabled(GL_BLEND) == GL_TRUE;
    GLint      blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    // Same one-shot treatment for the scissor test (a stray scissor from
    // another draw would clip the fullscreen quads) and the caller's FBO +
    // viewport (the raw passes below bind their own targets). currentFB can
    // legitimately be null outside a pass — guard the restore.
    const bool scissorBefore = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glDisable(GL_SCISSOR_TEST);
    const auto callerFB = g_pHyprRenderer->m_renderData.currentFB;
    GLint      callerVp[4];
    glGetIntegerv(GL_VIEWPORT, callerVp);

    // Radius is relative to the bottom buffer's actual texel size, which is the ACHIEVED
    // downscale (1 << m_levels — the nearest power of two to the configured `resolution`,
    // see ensureScratch()), not the raw config value, so the gaussian stage's footprint
    // stays consistent with the buffer it's actually running on.
    const float    strengthRadius = std::max(0.5F, (m_sizePx * m_strength) / static_cast<float>(1 << m_levels));
    const Vector2D blurSize{static_cast<float>(m_blurW), static_cast<float>(m_blurH)};

    // 1) Dual-Kawase down-chain: source -> L1 -> L2 -> ... -> fbA (bottom). Each step is a
    // proper 2x box/tent filter instead of one aliasing single jump (see blur.hpp for the
    // full rationale) — resolution=1 (m_levels==0) has no levels to walk at all, so fbA is
    // just a plain copy of the full-res source, matching the old single-scale behaviour.
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

    // 2) separable gaussian passes at the bottom of the pyramid, ping-pong between fbA/fbB
    // (unchanged from the single-scale version — cheap here since the buffer is small
    // regardless of output resolution).
    for (int p = 0; p < m_passes; ++p) {
        gaussPass(m_fbA->getTexture(), m_fbB, blurSize, {1.0F, 0.0F}, strengthRadius); // H: A->B
        gaussPass(m_fbB->getTexture(), m_fbA, blurSize, {0.0F, 1.0F}, strengthRadius); // V: B->A
    }

    // 3) Dual-Kawase up-chain: fbA (bottom, post-gaussian) -> ... -> L1 -> dst (full res,
    // opaque). Walked in reverse, reusing the SAME FBOs the down-chain used for each level —
    // by the time we write into a level here its down-chain content is no longer needed, so
    // this is the total extra scratch memory the pyramid costs over the old single buffer.
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

    // The dim colour is NOT composited here: the cache this writes must stay
    // independent of backdrop_color (it is part of the per-frame key now) and
    // of the animation — renderBackdrop() draws the dim rect itself, faded by
    // the same curve as the blit alpha.

    // Restore everything captured above: blend func + enable bit (the passes
    // ran with blending disabled for their own opaque draws), scissor test,
    // caller's FBO binding and viewport.
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

} // namespace gloview
