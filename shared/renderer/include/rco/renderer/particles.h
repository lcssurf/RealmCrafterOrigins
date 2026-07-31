#pragma once

#include <vector>
#include <cstdint>
#include <glad/glad.h>
#include <glm/glm.hpp>

namespace rco::renderer {

struct FXParams {
    int      burstCount = 0;
    float    streamInterval = 0.04f;
    float    lifetimeSeconds = 1.0f;

    float    speedMin = 1.0f;
    float    speedMax = 3.0f;
    glm::vec3 velBias = {0.f, 2.f, 0.f};
    float    velSpread = 0.5f;

    glm::vec4 colorStart = {1.f, 0.5f, 0.f, 1.f};
    glm::vec4 colorEnd = {1.f, 0.f, 0.f, 0.f};
    float    sizeStart = 8.f;
    float    sizeEnd = 2.f;
};

// Stable per-emitter identifier, returned by SpawnEmitterParams and consumed
// by RemoveEmitter — lets a caller that knows it created a PERMANENT emitter
// (duration<0, never self-expires) remove it explicitly instead of it living
// forever (e.g. a zone-anchored emitter that must stop when the player
// leaves that area — see zone_emitters_spawned_this_area/removal in
// main.cpp). 0 is reserved as "invalid/none" so a default-constructed
// handle is always safely distinguishable from a real one.
using EmitterHandle = uint64_t;
constexpr EmitterHandle kInvalidEmitterHandle = 0;

class ParticleSystem {
public:
    void Init();
    void Shutdown();

    // duration: 0 = burst only (no streaming after the initial burst, if
    // any — see FXParams::burstCount), >0 = streams for that many seconds
    // then stops, <0 (use -1) = streams forever, never expires — e.g. a
    // permanent zone-anchored emitter (torch, portal swirl) with no
    // caster/spell tied to its lifetime. See Update()'s emitterAlive check.
    // Returns a handle usable with RemoveEmitter — every spawn gets one
    // (finite spell emitters can just discard it, exactly like today).
    EmitterHandle SpawnEmitterParams(const FXParams& params,
                           glm::vec3 pos,
                           float now,
                           float duration = 0.f);

    // Removes the emitter immediately (its particles are dropped too — this
    // is "delete now", not "let it expire naturally"). No-op (returns false)
    // if the handle doesn't match any currently-active emitter, which is the
    // normal case for a handle whose emitter already self-expired on its
    // own — callers don't need to guard against that first.
    bool RemoveEmitter(EmitterHandle handle);

    void Update(float now, float dt);
    void Render(const glm::mat4& view, const glm::mat4& proj);

    // Diagnostic only — e.g. confirming a SpawnEmitterParams call actually
    // grew the active list (see kPZoneEmitters handler in main.cpp).
    size_t EmitterCount() const { return emitters_.size(); }

private:
    struct Particle {
        glm::vec3 pos;
        glm::vec3 vel;
        glm::vec4 colorStart;
        glm::vec4 colorEnd;
        float     sizeStart;
        float     sizeEnd;
        float     age;
        float     lifetime;
    };

    struct Emitter {
        EmitterHandle id;        // stable handle, see EmitterHandle above
        FXParams    params;
        glm::vec3   pos;
        float       startTime;
        float       duration;    // 0 = burst only, >0 = seconds until stream stops, <0 = never expires (see Update())
        float       nextSpawn;   // absolute time of next particle
        std::vector<Particle> particles;
    };

    void  spawnParticle(Emitter& e, float now);
    float spawnInterval(const Emitter& e) const;

    std::vector<Emitter> emitters_;
    EmitterHandle nextEmitterId_ = 1; // 0 reserved for kInvalidEmitterHandle

    // OpenGL: vec3 pos + vec4 color + float size = 8 floats per vertex
    struct Vertex { float x, y, z,  r, g, b, a,  s; };
    std::vector<Vertex> verts_;

    // Shader is registered in compile_shaders.cpp as "particle" and accessed
    // via Shader::shaders["particle"] at render time.
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
};

} // namespace rco::renderer
