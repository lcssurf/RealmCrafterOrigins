#include "rco/renderer/ripple_sim.h"
#include "rco/renderer/shader.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace rco::renderer {

void RippleSim::Init(int size, float windowSize) {
    if (tex_[0]) return;  // idempotent
    size_ = size;
    windowSize_ = windowSize;

    for (int i = 0; i < 2; ++i) {
        glCreateTextures(GL_TEXTURE_2D, 1, &tex_[i]);
        glTextureStorage2D(tex_[i], 1, GL_R32F, size_, size_);
        // GL_NEAREST (not LINEAR) — this is a discrete simulation, and the
        // update shader's same-texel self-read (u_previous, see
        // ripple_update.fs) needs each fragment to land exactly on one
        // source texel, not an interpolated blend of neighbors.
        glTextureParameteri(tex_[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(tex_[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // GL_CLAMP_TO_EDGE — the texture OBJECT's own baked-in default,
        // used as a fallback for any sampling that doesn't have a sampler
        // object explicitly bound to override it (see samplerEdge_/
        // samplerBorder_ below, and BorderSampler()'s doc in the header for
        // why a texture-level GL_CLAMP_TO_BORDER was tried and reverted —
        // it broke ripple_update.fs's own internal neighbor reads, since
        // glTextureParameteri state applies to EVERY consumer of the
        // texture, not just water.vs/water.fs). CLAMP_TO_EDGE here is the
        // "simulation-safe" choice: if some future code path samples this
        // texture without binding either explicit sampler, it degrades to
        // the old (mild edge-leak) behavior rather than killing the wave.
        glTextureParameteri(tex_[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex_[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // glTextureStorage2D doesn't clear the texture — zero it explicitly
        // so the first few frames don't show stale/garbage GPU memory as a
        // starting height field.
        std::vector<float> zeros(static_cast<size_t>(size_) * size_, 0.0f);
        glTextureSubImage2D(tex_[i], 0, 0, 0, size_, size_, GL_RED, GL_FLOAT, zeros.data());

        glCreateFramebuffers(1, &fbo_[i]);
        glNamedFramebufferTexture(fbo_[i], GL_COLOR_ATTACHMENT0, tex_[i], 0);
        glNamedFramebufferDrawBuffer(fbo_[i], GL_COLOR_ATTACHMENT0);
        if (glCheckNamedFramebufferStatus(fbo_[i], GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[ripple] FBO %d incomplete\n", i);
        }
    }

    // Scratch buffer for ShiftBuffer()'s 3-pass window-recenter shift — same
    // format/filtering as tex_[]. Not part of the ping-pong rotation itself
    // (cur_/dst only ever index tex_[]), just temporary storage so a
    // recenter can shift both buffers without one clobbering the other's
    // not-yet-read data (see ShiftBuffer's doc, header).
    glCreateTextures(GL_TEXTURE_2D, 1, &texShift_);
    glTextureStorage2D(texShift_, 1, GL_R32F, size_, size_);
    glTextureParameteri(texShift_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(texShift_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(texShift_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texShift_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    {
        std::vector<float> zeros(static_cast<size_t>(size_) * size_, 0.0f);
        glTextureSubImage2D(texShift_, 0, 0, 0, size_, size_, GL_RED, GL_FLOAT, zeros.data());
    }
    glCreateFramebuffers(1, &fboShift_);
    glNamedFramebufferTexture(fboShift_, GL_COLOR_ATTACHMENT0, texShift_, 0);
    glNamedFramebufferDrawBuffer(fboShift_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(fboShift_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[ripple] shift FBO incomplete\n");
    }

    glCreateVertexArrays(1, &dummyVao_);
    cur_ = 0;

    // Two sampler objects, DECOUPLED from the textures' own wrap state —
    // see BorderSampler()'s doc (header) for why a shared texture-level
    // wrap mode can't serve both consumers at once. Sampler objects (when
    // bound to a texture unit via glBindSampler) override whatever the
    // sampled texture's own glTextureParameteri wrap mode is, for that
    // unit's sampling only — same physical GL_R32F texture, different
    // wrap behavior depending on who's reading it.
    glCreateSamplers(1, &samplerEdge_);
    glSamplerParameteri(samplerEdge_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(samplerEdge_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(samplerEdge_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(samplerEdge_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateSamplers(1, &samplerBorder_);
    glSamplerParameteri(samplerBorder_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(samplerBorder_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(samplerBorder_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glSamplerParameteri(samplerBorder_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float kBorderColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glSamplerParameterfv(samplerBorder_, GL_TEXTURE_BORDER_COLOR, kBorderColor);
}

void RippleSim::UpdateWindow(glm::vec2 playerXZ, float windowSize) {
    // windowSize is refreshed every call, unconditionally (not gated by
    // hysteresis like windowCenter_ below — it's a scale derived from the
    // player's live ModelHeight(), not a position, so there's no "jump"
    // artifact to avoid by delaying the update). Guard against a
    // degenerate value collapsing WorldToUV()'s division: if windowSize
    // isn't sane, keep whatever windowSize_ already was rather than
    // adopting 0/negative.
    if (windowSize > 0.01f) {
        windowSize_ = windowSize;
    }

    // Hysteresis: only recenter once the player has drifted far enough
    // from the CURRENT window center, not every frame (which would make
    // WorldToUV()'s mapping — and therefore every vertex's ripple sample —
    // shift every single frame, defeating the point of a "window" at all).
    // 0.25 * windowSize_ matches the threshold picked in the Phase (a)
    // investigation. On recenter, buffer contents ARE shifted (see the
    // pendingShift_/ShiftBuffer() flow below) — an earlier version left
    // them as-is (accepted "jump" artifact), which broke continuous-walking
    // trails; see doc/TECH_DEBT.md #118.
    if (!windowInitialized_) {
        // First call ever — snap immediately instead of waiting for the
        // hysteresis threshold, so the window starts centered on the
        // player rather than at world origin (which could be arbitrarily
        // far away, or coincidentally close, in which case a naive
        // "windowCenter_ == (0,0)" check would misfire on later frames too).
        windowCenter_ = playerXZ;
        windowInitialized_ = true;
        return;
    }
    float dist = glm::length(playerXZ - windowCenter_);
    if (dist > 0.25f * windowSize_) {
        glm::vec2 delta = playerXZ - windowCenter_;  // new center - old center
        windowCenter_ = playerXZ;

        // Flag the shift for Update() to consume this frame, BEFORE its
        // normal propagation pass — see ShiftBuffer's doc (header) for the
        // full 3-pass reasoning. Derivation of the offset sign: for a
        // world point p, oldUV = (p-oldCenter)/windowSize_+0.5 and
        // newUV = (p-newCenter)/windowSize_+0.5 = oldUV - delta/windowSize_.
        // ShiftBuffer/ripple_shift.fs samples srcUV = dstUV - u_offsetUV,
        // and we want dstUV(=newUV)'s sample to come from oldUV, i.e.
        // oldUV = newUV + delta/windowSize_ = newUV - (-delta/windowSize_)
        // — so u_offsetUV = -delta/windowSize_.
        pendingShift_ = true;
        pendingShiftOffsetUV_ = -delta / windowSize_;
    }
}

void RippleSim::Shutdown() {
    for (int i = 0; i < 2; ++i) {
        if (fbo_[i]) { glDeleteFramebuffers(1, &fbo_[i]); fbo_[i] = 0; }
        if (tex_[i]) { glDeleteTextures(1, &tex_[i]);      tex_[i] = 0; }
    }
    if (dummyVao_) { glDeleteVertexArrays(1, &dummyVao_); dummyVao_ = 0; }
    if (samplerEdge_)   { glDeleteSamplers(1, &samplerEdge_);   samplerEdge_   = 0; }
    if (samplerBorder_) { glDeleteSamplers(1, &samplerBorder_); samplerBorder_ = 0; }
    if (fboShift_) { glDeleteFramebuffers(1, &fboShift_); fboShift_ = 0; }
    if (texShift_) { glDeleteTextures(1, &texShift_);      texShift_ = 0; }
    size_ = 0;
    cur_  = 0;
    windowInitialized_ = false;
    pendingShift_ = false;
}

void RippleSim::Update(bool stamp, glm::vec2 stampUV, float stampRadius,
                        float stampStrength, float damping) {
    if (!tex_[0]) return;

    // Window recenter shift — runs BEFORE this frame's normal propagation
    // pass, only when UpdateWindow() flagged a recenter this frame (rare,
    // hysteresis-gated — see UpdateWindow's doc). Keeps existing ripples
    // attached to their world position across a recenter instead of
    // jumping to a stale texel.
    if (pendingShift_) {
        ShiftBuffer(pendingShiftOffsetUV_);
        pendingShift_ = false;
    }

    auto it = Shader::shaders.find("ripple_update");
    if (it == Shader::shaders.end() || !it->second) return;
    auto& shader = *it->second;

    // dst is the OLDER buffer (currently holding the "previous" step) —
    // it's about to be overwritten with the newly-computed height, then
    // becomes "current" for the next call. See ripple_update.fs for why
    // dst is bound as BOTH the render target and u_previous in this pass.
    const int dst = 1 - cur_;

    GLint prevFbo = 0, prevVao = 0, prevViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_[dst]);
    glViewport(0, 0, size_, size_);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    shader.Bind();
    glBindTextureUnit(0, tex_[cur_]);
    glBindTextureUnit(1, tex_[dst]);
    // Explicit edge-clamp sampler on BOTH units — this is what keeps the
    // simulation's own 4-neighbor reads (ripple_update.fs) stable at the
    // buffer's border, independent of whatever wrap mode water.vs/water.fs
    // needs when they later sample tex_[cur_] through a DIFFERENT sampler
    // (BorderSampler(), bound by WaterManager::Render on its own texture
    // unit). Without this, the texture object's own default would apply —
    // see the header's BorderSampler() doc for why that was the bug.
    glBindSampler(0, samplerEdge_);
    glBindSampler(1, samplerEdge_);
    shader.SetInt("u_current",  0);
    shader.SetInt("u_previous", 1);
    shader.SetVec2("u_texelSize", glm::vec2(1.0f / size_, 1.0f / size_));
    shader.SetFloat("u_damping", damping);
    shader.SetBool("u_stampEnabled", stamp);
    shader.SetVec2("u_stampUV", stampUV);
    shader.SetFloat("u_stampRadius", stampRadius);
    shader.SetFloat("u_stampStrength", stampStrength);

    glBindVertexArray(dummyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // dst now holds the newest step — it becomes "current" for next frame,
    // and cur_ (soon to be "previous") is the one the NEXT call will
    // overwrite. This is the ping-pong swap.
    cur_ = dst;

    // Unbind the edge-clamp sampler so units 0/1 fall back to whatever
    // texture object is next bound there using ITS own default state —
    // leaving samplerEdge_ bound would silently override the wrap mode for
    // anything else that reuses units 0/1 later this frame (e.g. unit 1 is
    // also used by WaterManager::Render for gDepth_ — harmless there since
    // screen-space UVs never leave [0,1], but leaving stray sampler state
    // bound across unrelated draws is fragile and worth avoiding).
    glBindSampler(0, 0);
    glBindSampler(1, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glBindVertexArray(static_cast<GLuint>(prevVao));
}

void RippleSim::ShiftBuffer(glm::vec2 offsetUV) {
    auto it = Shader::shaders.find("ripple_shift");
    if (it == Shader::shaders.end() || !it->second) return;
    auto& shader = *it->second;

    const int dst = 1 - cur_;

    GLint prevFbo = 0, prevVao = 0, prevViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glViewport(0, 0, size_, size_);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(dummyVao_);

    shader.Bind();
    shader.SetInt("u_src", 0);
    glBindSampler(0, samplerEdge_);  // out-of-range reads inside ripple_shift.fs are handled explicitly (zero), this is just for in-range sampling precision (GL_NEAREST, matches every other read of this texture)

    // Pass 1: scratch = shifted(tex_[cur_] original) — read tex_[cur_]
    // BEFORE it's overwritten in pass 2 below.
    glBindFramebuffer(GL_FRAMEBUFFER, fboShift_);
    glBindTextureUnit(0, tex_[cur_]);
    shader.SetVec2("u_offsetUV", offsetUV);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Pass 2: tex_[cur_] = shifted(tex_[dst] original) — safe to overwrite
    // tex_[cur_] now, its data is already preserved (shifted) in scratch.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_[cur_]);
    glBindTextureUnit(0, tex_[dst]);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Pass 3: tex_[dst] = scratch (plain copy — scratch already has the
    // offset applied from pass 1, so u_offsetUV=0 here).
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_[dst]);
    glBindTextureUnit(0, texShift_);
    shader.SetVec2("u_offsetUV", glm::vec2(0.0f));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindSampler(0, 0);

    // After the 3 passes: physical slot `dst` holds the correctly-shifted
    // "current" data (from pass 1, via scratch) and physical slot `cur_`
    // holds the correctly-shifted "previous" data (from pass 2) — flip
    // cur_ to match this new physical layout (see header doc).
    cur_ = dst;

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glBindVertexArray(static_cast<GLuint>(prevVao));
}

void RippleSim::DebugDraw(int viewportW, int viewportH) const {
    if (!tex_[0]) return;

    auto it = Shader::shaders.find("fstexture");
    if (it == Shader::shaders.end() || !it->second) return;
    auto& shader = *it->second;

    GLint prevVao = 0, prevViewport[4] = {};
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    // Small "minimap" corner, top-left — fullscreen_tri.vs always covers
    // the CURRENT viewport, so restricting the viewport to a small
    // sub-rectangle before the draw is what confines it to a corner
    // instead of covering the whole screen.
    int debugSize = std::min(viewportH / 6, 220);
    glViewport(8, viewportH - debugSize - 8, debugSize, debugSize);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    (void)viewportW;  // corner position only depends on height (top-left, fixed X offset)

    shader.Bind();
    shader.SetInt("u_tex", 0);
    glBindTextureUnit(0, tex_[cur_]);

    glBindVertexArray(dummyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glBindVertexArray(static_cast<GLuint>(prevVao));
}

} // namespace rco::renderer
