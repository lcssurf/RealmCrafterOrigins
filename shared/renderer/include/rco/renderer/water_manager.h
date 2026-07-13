#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rco::renderer {

class Texture2D;
class Pipeline;
class RippleSim;

// One static water plane (Phase 0 — flat textured quad, no waves/depth/
// reflection yet) as received from the server via PZoneWater — see
// server/internal/world/frame.go WaterPayload and
// tools/gue/src/zone_scene.h ZWater (the authoring side, Zone editor).
struct WaterEntry {
    glm::vec3   pos{0.f};
    glm::vec2   scale{16.f, 16.f};                        // full width/depth, world units (X/Z)
    glm::vec3   color{0.f, 0.39f, 0.59f};
    float       opacity  = 0.5f;                          // 0-1
    std::string texPath;                                  // relative to dist/client/, "" = flat tint only
    float       texScale = 15.f;                          // texture tiling repeat count
    // Gerstner wave tunables (real vertex displacement, water.vs) — primary
    // wave direction/wavenumber/speed. waveDir is X/Z, normalized in-shader.
    // wave_scale is interpreted directly as the wave's angular wavenumber k
    // (rad/world-unit): wavelength = 2*PI/waveScale.
    float       waveSpeed = 0.3f;
    glm::vec2   waveDir{0.7071f, 0.7071f};
    float       waveScale = 0.35f;
    // Sub-fase 2a — depth-based transparency (water.fs samples gDepth_ via
    // Pipeline::SceneDepthTexture()). shallowColor/deepColor replace the
    // old flat `color` field's role in the lit shading.
    glm::vec3   shallowColor{0.3f, 0.7f, 0.6f};
    glm::vec3   deepColor{0.02f, 0.10f, 0.20f};
    float       depthFadeDistance = 2.5f;
    // Sub-fase 2b — procedural shoreline foam. Reuses shallowColor/deepColor's
    // depthDiff comparison (water.fs) — no separate depth field here.
    float       foamWidth = 0.4f;
    glm::vec3   foamColor{1.f, 1.f, 1.f};

    // Phase (b) — player-ripple gating. AABB test in the water plane's own
    // XZ footprint (pos.xz +/- scale/2) — does NOT check Y; callers combine
    // this with a separate surface-height check (see WaterManager::Render)
    // since "is the player near the water's Y" is a judgment call that
    // varies by use (ripple gating uses a generous tolerance; a future
    // swim-detection mechanic would need a different one).
    bool Contains(glm::vec2 xz) const {
        glm::vec2 half = scale * 0.5f;
        glm::vec2 rel  = xz - glm::vec2(pos.x, pos.z);
        return rel.x >= -half.x && rel.x <= half.x &&
               rel.y >= -half.y && rel.y <= half.y;
    }
};

// Holds the current area's static water planes and draws them every frame
// as a forward-pass, alpha-blended grid mesh — reuses the same forward-pass
// slot as ParticleSystem::Render (see Pipeline::End's forward_pass callback
// in pipeline.cpp), so it gets scene-depth-tested blending "for free" from
// the existing deferred + forward split.
//
// Real Gerstner-wave vertex displacement (water.vs) + Blinn-Phong specular
// against the analytically-derived wave normal (water.fs). No depth-based
// transparency/shoreline, no reflection/refraction yet. See
// doc/TECH_DEBT.md #117 for later phases and the swim-detection mechanic.
class WaterManager {
public:
    // Creates the quad VAO — call once after the GL context is ready.
    void Init();
    void Shutdown();

    // Replaces the full water list — call when a PZoneWater packet arrives.
    void SetWater(std::vector<WaterEntry> water) { water_ = std::move(water); }

    // Drops all water — call on area/logout/disconnect so a stray leftover
    // plane from the previous area can't render for a frame or two before
    // the new area's PZoneWater packet (if any) arrives.
    void Clear() { water_.clear(); }

    size_t Count() const { return water_.size(); }

    // Phase (b) — tests whether `playerPos` is standing in ANY water
    // instance: WaterEntry::Contains() (XZ footprint) AND vertically WITHIN
    // that instance's water volume (PlayerWithinWaterVolume(), .cpp — a
    // small allowance above the surface for shoreline wading down to a
    // generous max-submersion depth, NOT a symmetric "close to the surface"
    // band, so it still covers a fully submerged player). Callers (main.cpp) use this BEFORE
    // RippleSim::Update() runs, to decide whether to trigger an automatic
    // stamp this frame — Render() below independently re-runs the same
    // per-instance test to decide which instance gets u_hasRipple=1 (kept
    // separate/stateless rather than threading the result through, since
    // the two calls happen at different points in the frame and Render()
    // needs the per-instance answer anyway, not just a single bool).
    bool PlayerContact(const glm::vec3& playerPos, WaterEntry* outEntry = nullptr) const;

    // Call from inside Pipeline::End's forward_pass callback, alongside
    // ParticleSystem::Render — shares the same depth-tested, blended slot.
    // `pipeline` is only used to read the current sun (SunDirection()/
    // SunColor()) for the shader's basic sun-only lighting term — not a
    // second source of truth, just piping the existing value through.
    // `timeSeconds` drives the Gerstner wave animation — pass the caller's
    // existing wall-clock time (e.g. main.cpp's `now`, glfwGetTime() based),
    // not a new clock owned by this class.
    // `playerPos`/`rippleSim` (Phase b): only the water instance containing
    // playerPos (via WaterEntry::Contains + Y tolerance, same test as
    // PlayerContact) receives the ripple contribution (u_hasRipple=1 +
    // texture bind + window uniforms) — every other instance gets
    // u_hasRipple=0 and the shader skips sampling the ripple texture
    // entirely (zero added cost). Pass rippleSim=nullptr to disable ripple
    // integration outright (e.g. before RippleSim::Init() has run).
    // `rippleHeightScale`: derived by the caller from the player's live
    // ModelHeight() (see main.cpp's kRippleMaxDisplacementFraction) — a
    // safety ceiling so peak ripple displacement stays a small fraction of
    // the character's own height instead of a fixed magic number. Only
    // used/uploaded when a given instance actually has u_hasRipple=1.
    // `rippleFoamThresholdLow/High`: DIRECT thresholds (smoothstep) applied
    // to abs(rippleH) — the raw ripple height field sampled a second time in
    // water.fs, the SAME value/texture the Phase (a) F9/F10 debug view
    // already validated visually. Independent of WaterEntry::foamWidth
    // (shoreline/terrain foam, untouched) and of depthDiff entirely — no
    // interaction with the Sub-fase 2b foam system. See main.cpp's
    // kRippleFoamThresholdLow/High.
    void Render(const glm::mat4& view, const glm::mat4& proj, const Pipeline& pipeline,
                float timeSeconds, const glm::vec3& playerPos, const RippleSim* rippleSim,
                float rippleHeightScale, float rippleFoamThresholdLow, float rippleFoamThresholdHigh);

private:
    Texture2D* getOrLoadTexture(const std::string& texPath);

    std::vector<WaterEntry> water_;
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    int    idxCount_ = 0;  // triangle index count for the subdivided grid

    // Texture cache keyed by (client-relative) texPath — avoids reloading
    // from disk every frame for repeated water instances sharing a texture.
    std::unordered_map<std::string, std::unique_ptr<Texture2D>> textures_;
};

} // namespace rco::renderer
