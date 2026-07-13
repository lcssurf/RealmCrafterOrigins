#include "rco/renderer/water_manager.h"
#include "rco/renderer/shader.h"
#include "rco/renderer/texture.h"
#include "rco/renderer/pipeline.h"
#include "rco/renderer/ripple_sim.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rco::renderer {

namespace {
// Subdivided grid resolution — enough vertices for Gerstner waves (water.vs)
// to show real curvature (a 4-vertex quad can't bend) without wasting
// vertices for a plane that's typically 16x16 to a few dozen world units.
// 24x24 = 576 vertices / 1058 triangles per water instance — trivial for
// the GPU per instance (see cost note in WaterManager::Render).
constexpr int kWaterGridN = 24;

// Phase (b) — vertical "is the player in this water" gate for ripple
// purposes. Originally a single symmetric tolerance
// (abs(playerPos.y - w.pos.y) < 1.5) — that assumed the player is always
// NEAR the surface (wading), so it broke for a fully submerged player: once
// playerPos.y sinks more than 1.5 units below the surface, the old test
// failed and u_hasRipple dropped to 0 even though the player was still
// in the water, just deeper. Split into two independent bounds instead of
// one symmetric distance, since "how far above the surface still counts"
// and "how far below still counts" are NOT the same question:
//   - kSurfaceYToleranceAbove: small allowance for standing right at the
//     shoreline edge (feet at/slightly above the surface Y — the water
//     surface itself bobs via Gerstner+ripple, so a strict >= would flicker
//     in/out every frame). A tighter 0.15 was tried and reverted — it broke
//     confirmed-working surface/submerged behavior in practice, so this is
//     back to 0.5 (accepting that it can activate slightly above the
//     surface in some edge cases, e.g. a modestly raised ledge/rock) until
//     a replacement fix is worked out.
//   - kMaxSubmersionDepth: generous cap on how far BELOW the surface still
//     counts as "in the water" — covers a fully submerged swim (the bug
//     this fixes) without activating the ripple contribution at
//     unreasonable depths (e.g. the floor of a very deep lake/ocean water
//     instance) — there's no per-instance floor/depth data available here
//     to derive an exact value from, so this is a deliberately generous,
//     fixed constant (not tied to ModelHeight() — the "how deep is the
//     water" question is unrelated to the player's own size) rather than a
//     precise swim-depth mechanic (that's still the separate,
//     not-yet-implemented mechanic noted in doc/TECH_DEBT.md #117).
constexpr float kSurfaceYToleranceAbove = 0.5f;
constexpr float kMaxSubmersionDepth     = 5.0f;

// True when playerPos is vertically WITHIN this water instance's volume:
// at or slightly above the surface (shoreline/wading) down to
// kMaxSubmersionDepth below it (fully submerged) — NOT a symmetric "close
// to the surface" band, so it stays true regardless of how deep the player
// swims (up to the cap), fixing the "ripple stops when fully submerged" bug
// while still excluding a player standing well above the water (e.g. on a
// nearby rock/ledge).
bool PlayerWithinWaterVolume(const glm::vec3& playerPos, const WaterEntry& w) {
    return playerPos.y <= w.pos.y + kSurfaceYToleranceAbove &&
           playerPos.y >= w.pos.y - kMaxSubmersionDepth;
}

// Bug fix: rendering the ripple contribution used to be gated by
// "is the player RIGHT NOW standing in this water" (Contains() +
// PlayerWithinWaterVolume(), the live-position test above). That's correct
// for deciding whether to STAMP a new ripple (still used for that, see
// WaterManager::PlayerContact), but wrong for deciding whether to RENDER
// the ripples that already exist in the buffer: the instant the player
// steps out of the water (crosses the XZ footprint or the Y band), the live
// test flips false and u_hasRipple drops to 0 for every instance in the
// same frame — the water goes flat/loses its foam immediately, even though
// RippleSim::Update() keeps propagating/decaying the buffer underneath
// (it's not gated by water contact, see main.cpp) and there's real,
// still-visible energy left in it.
//
// Fix: gate RENDERING by whether the ripple simulation's WINDOW (which
// keeps following the player's XZ every frame, on land or not — see
// RippleSim::UpdateWindow, unconditional) still overlaps this water
// instance's XZ footprint, not by live player containment. This lets
// existing ripples keep rendering/decaying naturally for a bit after the
// player leaves — they only stop once the window has drifted far enough
// away (recenter hysteresis) or the buffer has damped them out, whichever
// comes first — instead of vanishing the instant the player's feet leave
// the water. XZ-only (no Y term): the window itself doesn't track a Y, and
// a ripple already in the buffer doesn't care where the player currently
// is vertically, only whether it's spatially still under this water plane.
bool RippleWindowOverlapsWater(const WaterEntry& w, glm::vec2 windowCenter, float windowSize) {
    glm::vec2 waterHalf = w.scale * 0.5f;
    glm::vec2 waterMin  = glm::vec2(w.pos.x, w.pos.z) - waterHalf;
    glm::vec2 waterMax  = glm::vec2(w.pos.x, w.pos.z) + waterHalf;
    float windowHalf = windowSize * 0.5f;
    glm::vec2 winMin = windowCenter - glm::vec2(windowHalf);
    glm::vec2 winMax = windowCenter + glm::vec2(windowHalf);
    return waterMin.x <= winMax.x && waterMax.x >= winMin.x &&
           waterMin.y <= winMax.y && waterMax.y >= winMin.y;
}
}  // namespace

void WaterManager::Init() {
    if (vao_) return;  // idempotent

    // NxN grid in the XZ plane, local space [-0.5,0.5]^2, y=0 at rest.
    // Interleaved [pos.xyz | uv.xy] — stride 20 bytes (same layout as
    // before, just many more vertices). Same grid resolution/layout as the
    // GUE's water mesh (tools/gue/src/zone_renderer.cpp BuildWaterQuadVAO) —
    // Gerstner waves (water.vs) displace these in world space every frame.
    std::vector<float> verts;
    verts.reserve(kWaterGridN * kWaterGridN * 5);
    for (int gz = 0; gz < kWaterGridN; ++gz) {
        for (int gx = 0; gx < kWaterGridN; ++gx) {
            float u = (float)gx / (float)(kWaterGridN - 1);
            float v = (float)gz / (float)(kWaterGridN - 1);
            verts.push_back(u - 0.5f); // x
            verts.push_back(0.f);      // y
            verts.push_back(v - 0.5f); // z
            verts.push_back(u);        // u
            verts.push_back(v);        // v
        }
    }
    std::vector<uint16_t> idx;
    idx.reserve((kWaterGridN - 1) * (kWaterGridN - 1) * 6);
    for (int gz = 0; gz < kWaterGridN - 1; ++gz) {
        for (int gx = 0; gx < kWaterGridN - 1; ++gx) {
            uint16_t i0 = (uint16_t)(gz * kWaterGridN + gx);
            uint16_t i1 = (uint16_t)(gz * kWaterGridN + gx + 1);
            uint16_t i2 = (uint16_t)((gz + 1) * kWaterGridN + gx + 1);
            uint16_t i3 = (uint16_t)((gz + 1) * kWaterGridN + gx);
            idx.push_back(i0); idx.push_back(i1); idx.push_back(i2);
            idx.push_back(i0); idx.push_back(i2); idx.push_back(i3);
        }
    }
    // Winding doesn't matter — Render() disables GL_CULL_FACE for water
    // entirely (see the backface-culling fix earlier this session).
    idxCount_ = (int)idx.size();

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glCreateBuffers(1, &ebo_);
    glNamedBufferData(vbo_, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glNamedBufferData(ebo_, (GLsizeiptr)(idx.size() * sizeof(uint16_t)), idx.data(), GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, 20);
    glVertexArrayElementBuffer(vao_, ebo_);
    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao_, 0, 0);
    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 2, GL_FLOAT, GL_FALSE, 12);
    glVertexArrayAttribBinding(vao_, 1, 0);
}

void WaterManager::Shutdown() {
    if (vbo_) { glDeleteBuffers(1, &vbo_);      vbo_ = 0; }
    if (ebo_) { glDeleteBuffers(1, &ebo_);      ebo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    textures_.clear();
    water_.clear();
}

Texture2D* WaterManager::getOrLoadTexture(const std::string& texPath) {
    if (texPath.empty()) return nullptr;
    auto it = textures_.find(texPath);
    if (it != textures_.end()) return it->second->Valid() ? it->second.get() : nullptr;

    // Client's cwd is dist/client/ itself, so texPath (already relative to
    // dist/client/, same convention as WorldObject::ModelPath) is used as-is
    // — no ResolveClientAsset-style "../client/" prefix needed here (that's
    // GUE-only, since the GUE's cwd is dist/tools/).
    TextureCreateInfo info;
    info.path = texPath;
    info.sRGB = true;  // albedo colour texture
    auto tex = std::make_unique<Texture2D>(info);
    Texture2D* raw = tex.get();
    bool valid = tex->Valid();
    textures_[texPath] = std::move(tex);
    return valid ? raw : nullptr;
}

bool WaterManager::PlayerContact(const glm::vec3& playerPos, WaterEntry* outEntry) const {
    for (const auto& w : water_) {
        if (w.Contains({playerPos.x, playerPos.z}) &&
            PlayerWithinWaterVolume(playerPos, w)) {
            if (outEntry) *outEntry = w;
            return true;
        }
    }
    return false;
}

void WaterManager::Render(const glm::mat4& view, const glm::mat4& proj, const Pipeline& pipeline,
                           float timeSeconds, const glm::vec3& playerPos, const RippleSim* rippleSim,
                           float rippleHeightScale, float rippleFoamThresholdLow, float rippleFoamThresholdHigh) {
    if (water_.empty() || !vao_) return;

    // [DIAGNOSTIC] Investigating "distant water still rippling" — confirms
    // (a) how many water instances exist this session (a single huge
    // instance vs. multiple small ones changes the diagnosis, see the
    // investigation report) and (b) whether Contains()/u_hasRipple is
    // actually 0 for every instance except the correct one, isolating a
    // possible per-instance gating bug from the GL_CLAMP_TO_EDGE
    // border-leak hypothesis (ripple_sim.cpp:27-28). Logged once every ~2s
    // (not every frame) to avoid flooding stderr.
    {
        static float lastLogTime = -1000.0f;
        if (timeSeconds - lastLogTime > 2.0f) {
            lastLogTime = timeSeconds;
            std::fprintf(stderr, "[water-ripple] frame check: %zu water instance(s), playerPos=(%.2f,%.2f,%.2f)\n",
                         water_.size(), playerPos.x, playerPos.y, playerPos.z);
            for (const auto& w : water_) {
                bool windowOverlap = rippleSim != nullptr &&
                    RippleWindowOverlapsWater(w, rippleSim->WindowCenter(), rippleSim->WindowSize());
                std::fprintf(stderr,
                    "[water-ripple]   instance pos=(%.1f,%.1f,%.1f) scale=(%.1f,%.1f) "
                    "WindowOverlap=%s -> u_hasRipple=%d\n",
                    w.pos.x, w.pos.y, w.pos.z, w.scale.x, w.scale.y,
                    windowOverlap ? "true" : "false", windowOverlap ? 1 : 0);
            }
        }
    }

    auto it = Shader::shaders.find("water");
    if (it == Shader::shaders.end() || !it->second) return;
    auto& shader = *it->second;

    shader.Bind();
    glm::mat4 vp = proj * view;
    // u_viewProj is the same for every water instance this frame (unlike
    // u_model, which varies per instance) — set once. Gerstner displacement
    // (water.vs) needs it separate from u_model: wave phase is evaluated on
    // the true world-space rest position (u_model * aPos) BEFORE the camera
    // transform, so a pre-baked per-instance MVP can't be used here.
    shader.SetMat4("u_viewProj", vp);

    // Sun-only lighting — read from the pipeline's existing sun (same value
    // globalLightPass_() uses for gPhongGlobal.fs), not a duplicated source.
    // No shadow — see doc/TECH_DEBT.md #117. Approximate specular IBL
    // reflection added in Fase 3 (below); no diffuse IBL/ambient term here.
    shader.SetVec3("u_sunDir",   pipeline.SunDirection());
    shader.SetVec3("u_sunColor", pipeline.SunColor());
    // Camera world position — same value/source of truth every deferred
    // pass already uploads as "u_viewPos" (Pipeline::camPos_, set every
    // frame by Begin()). Needed for the Blinn-Phong specular view vector.
    shader.SetVec3("u_viewPos", pipeline.ViewPos());

    // Sub-fase 2a — depth-based transparency. u_invViewProj mirrors every
    // deferred pass's "glm::inverse(viewProj_)" pattern (pipeline.cpp:642
    // etc), computed here since vp is already being built for u_viewProj.
    // gDepth_ bound to unit 1 (unit 0 is the per-instance albedo texture,
    // bound inside the loop below) via Pipeline::SceneDepthTexture() — same
    // texture the light passes already sample, not a copy.
    shader.SetMat4("u_invViewProj", glm::inverse(vp));
    glBindTextureUnit(1, pipeline.SceneDepthTexture());
    shader.SetInt("u_sceneDepth", 1);

    // Blinn-Phong specular tuning — fixed for now (not exposed as GUE
    // sliders in this fix; power=80 sits in the requested 64-128 "tight
    // highlight" range, intensity=1.75 in the requested 1.5-2.0 range).
    shader.SetFloat("u_specPower",     80.0f);
    shader.SetFloat("u_specIntensity", 1.75f);

    // Fase 3 — approximate IBL reflection. Unit 3 (0=per-instance albedo,
    // 1=gDepth_, 2=ripple tex, all already taken — see the loop below and
    // Sub-fase 2a/ripple comments). Reuses the SAME already-baked
    // prefiltered specular cubemap gPhongGlobal.fs samples for its own IBL
    // (Pipeline::PrefilterCube() -> engine_->prefilterCube_) — not a copy,
    // not a new bake. u_reflectionStrength is a fixed GLOBAL constant (not
    // per-ZWater instance — this is an approximate/aesthetic effect, not a
    // configurable per-water-body property, see doc/TECH_DEBT.md), same
    // pattern as the fixed specPower/specIntensity just above. 0.45 gives a
    // visible "there's sky/environment in this water" sensation at grazing
    // angles without washing out the shallow/deep color gradient or the
    // sun specular highlight.
    glBindTextureUnit(3, pipeline.PrefilterCube());
    shader.SetInt("u_prefilterCube", 3);
    shader.SetFloat("u_reflectionStrength", 0.45f);

    // Gerstner wave clock. Caller-supplied (main.cpp's `now`, glfwGetTime()-
    // based) — not a clock owned by WaterManager.
    shader.SetFloat("u_time", timeSeconds);

    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);

    // Backface culling is on globally (Pipeline::RenderFrame sets
    // GL_CULL_FACE + GL_CCW before all passes, pipeline.cpp:946-947, and
    // the forward pass never disables it) while the water quad's winding
    // only faces "up" correctly from one side — the top face, the one the
    // default gameplay camera actually looks at, was being culled.
    // Disabling cull for water (instead of flipping the EBO winding) is
    // deliberate: it also makes the plane visible from underneath, which
    // will matter once underwater/swim camera views exist (see
    // doc/TECH_DEBT.md #117). Save/restore so it doesn't leak into whatever
    // the forward_pass callback draws after water (e.g. particles, if ever
    // reordered).
    GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);  // opaque-ish plane, but still forward/transparent slot — no depth write
    glBindVertexArray(vao_);

    for (const auto& w : water_) {
        glm::mat4 m = glm::translate(glm::mat4(1.f), w.pos);
        m = glm::scale(m, glm::vec3(w.scale.x, 1.f, w.scale.y));
        shader.SetMat4("u_model", m);
        shader.SetFloat("u_texScale", w.texScale);
        shader.SetVec4("u_tint", glm::vec4(w.color, w.opacity));
        shader.SetFloat("u_waveSpeed", w.waveSpeed);
        shader.SetVec2("u_waveDir",   w.waveDir);
        shader.SetFloat("u_waveScale", w.waveScale);
        shader.SetVec3("u_shallowColor", w.shallowColor);
        shader.SetVec3("u_deepColor",    w.deepColor);
        shader.SetFloat("u_depthFadeDistance", w.depthFadeDistance);
        shader.SetFloat("u_foamWidth", w.foamWidth);
        shader.SetVec3("u_foamColor", w.foamColor);

        // Phase (b) — ripple gating: NOT the same test as PlayerContact
        // (live player containment) anymore — that caused the ripple effect
        // to snap off the instant the player stepped out of the water, even
        // though the buffer still had real, decaying energy in it (see
        // RippleWindowOverlapsWater's doc, above). Gated instead by whether
        // the ripple simulation's WINDOW (which keeps following the
        // player's XZ every frame regardless of water contact) still
        // overlaps this instance's XZ footprint — existing ripples keep
        // rendering/fading naturally for a bit after leaving the water,
        // instead of vanishing immediately. water.vs/water.fs branch on
        // u_hasRipple and skip sampling u_rippleTex entirely when it's 0
        // (zero added cost for instances outside the window).
        bool hasRipple = rippleSim != nullptr &&
                         RippleWindowOverlapsWater(w, rippleSim->WindowCenter(), rippleSim->WindowSize());
        shader.SetInt("u_hasRipple", hasRipple ? 1 : 0);
        if (hasRipple) {
            shader.SetVec2("u_rippleWindowCenter", rippleSim->WindowCenter());
            shader.SetFloat("u_rippleWindowSize",  rippleSim->WindowSize());
            shader.SetVec2("u_rippleTexelSize", glm::vec2(1.0f / rippleSim->Size(), 1.0f / rippleSim->Size()));
            shader.SetFloat("u_rippleHeightScale", rippleHeightScale);
            shader.SetFloat("u_rippleFoamThreshold_low",  rippleFoamThresholdLow);
            shader.SetFloat("u_rippleFoamThreshold_high", rippleFoamThresholdHigh);
            glBindTextureUnit(2, rippleSim->CurrentTexture());
            // Explicit border-clamp sampler on unit 2, DISTINCT from the
            // edge-clamp sampler RippleSim::Update() binds internally on
            // units 0/1 for the simulation's own neighbor reads — same
            // physical texture, different wrap behavior for this
            // (water-side, window-relative) consumer. See
            // RippleSim::BorderSampler()'s doc for why the two can't share
            // a single texture-level wrap mode.
            glBindSampler(2, rippleSim->BorderSampler());
            shader.SetInt("u_rippleTex", 2);
        }

        Texture2D* tex = getOrLoadTexture(w.texPath);
        shader.SetInt("u_hasTex", tex ? 1 : 0);
        if (tex) {
            tex->Bind(0);
            shader.SetInt("u_tex", 0);
        }

        glDrawElements(GL_TRIANGLES, idxCount_, GL_UNSIGNED_SHORT, nullptr);
    }

    glDepthMask(GL_TRUE);
    if (cullWasEnabled) glEnable(GL_CULL_FACE);
    glBindVertexArray(static_cast<GLuint>(prevVao));
    // Unbind the border-clamp sampler from unit 2 — avoids leaving stray
    // sampler state bound across unrelated draws later in the frame (same
    // reasoning as RippleSim::Update()'s own sampler unbind).
    glBindSampler(2, 0);
}

} // namespace rco::renderer
