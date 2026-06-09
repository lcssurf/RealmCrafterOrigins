#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <glad/glad.h>
#include <glm/glm.hpp>

namespace rco::renderer {

enum class EmitterType : uint8_t {
    Fire      = 0,  // continuous rising flame
    Explosion = 1,  // one-shot outward burst (NPC death / impact)
    Heal      = 2,  // rising green sparkles
    Portal    = 3,  // spinning cyan ring
    Blood     = 4,  // short red splatter burst
    Smoke     = 5,  // rising grey wisps
};

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

// Resolves a soft path key from ability_templates into an existing emitter type.
// Known keys (case-insensitive): vfx:fire/explosion/heal/portal/blood/smoke
EmitterType ResolveVFXPathToType(const std::string& path, bool* resolved);

class ParticleSystem {
public:
    void Init();
    void Shutdown();

    void SpawnEmitterParams(const FXParams& params,
                           glm::vec3 pos,
                           float now,
                           float duration = 0.f);

    // Spawn an emitter at a world position.
    // duration > 0: streaming emitter (fire, portal).
    // duration == 0: one-shot burst (explosion, heal).
    void SpawnEmitter(EmitterType type, glm::vec3 pos, float now, float duration = 0.f);

    void Update(float now, float dt);
    void Render(const glm::mat4& view, const glm::mat4& proj);

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
        FXParams    params;
        glm::vec3   pos;
        float       startTime;
        float       duration;    // 0 = burst only
        float       nextSpawn;   // absolute time of next particle
        std::vector<Particle> particles;
    };

    void  spawnParticle(Emitter& e, float now);
    float spawnInterval(const Emitter& e) const;

    std::vector<Emitter> emitters_;

    // OpenGL: vec3 pos + vec4 color + float size = 8 floats per vertex
    struct Vertex { float x, y, z,  r, g, b, a,  s; };
    std::vector<Vertex> verts_;

    // Shader is registered in compile_shaders.cpp as "particle" and accessed
    // via Shader::shaders["particle"] at render time.
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
};

} // namespace rco::renderer
