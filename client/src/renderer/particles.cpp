#include "particles.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/random.hpp>
#include <algorithm>
#include <cstdlib>

namespace rco::renderer {

// ---------------------------------------------------------------------------
// Per-type configuration
// ---------------------------------------------------------------------------

namespace {

struct TypeCfg {
    glm::vec4 colorStart;
    glm::vec4 colorEnd;
    float     sizeStart;
    float     sizeEnd;
    float     lifetime;       // particle lifetime (seconds)
    float     speedMin;
    float     speedMax;
    glm::vec3 velBias;        // directional bias (e.g. upward)
    float     spread;         // random spread radius added to velBias
    int       burstCount;     // > 0: emit all at once; 0: stream
    float     streamInterval; // seconds between streamed particles
};

static const TypeCfg kCfg[] = {
    // Fire
    { {1.f,0.55f,0.05f,0.9f}, {0.8f,0.1f,0.f,0.f},
      8.f, 2.f, 1.2f, 1.5f, 3.0f, {0.f,2.5f,0.f}, 0.5f, 0, 0.04f },
    // Explosion
    { {1.f,0.6f,0.f,1.f}, {0.3f,0.f,0.f,0.f},
      12.f, 2.f, 0.8f, 3.0f, 7.0f, {0.f,1.0f,0.f}, 3.14159f, 40, 0.f },
    // Heal
    { {0.2f,1.f,0.4f,0.9f}, {0.f,0.5f,0.1f,0.f},
      6.f, 2.f, 1.4f, 1.0f, 2.5f, {0.f,2.0f,0.f}, 0.7f, 20, 0.f },
    // Portal
    { {0.f,0.8f,1.f,0.8f}, {0.f,0.2f,0.6f,0.f},
      5.f, 1.5f, 1.8f, 1.5f, 3.0f, {0.f,0.5f,0.f}, 3.14159f, 0, 0.06f },
    // Blood
    { {0.9f,0.f,0.f,1.f}, {0.4f,0.f,0.f,0.f},
      5.f, 1.f, 0.4f, 2.0f, 5.0f, {0.f,-1.5f,0.f}, 1.8f, 15, 0.f },
    // Smoke
    { {0.5f,0.5f,0.5f,0.5f}, {0.3f,0.3f,0.3f,0.f},
      10.f, 6.f, 2.0f, 0.3f, 0.8f, {0.f,1.2f,0.f}, 0.4f, 0, 0.12f },
};

static float frand() { return static_cast<float>(rand()) / RAND_MAX; }

} // namespace

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------

void ParticleSystem::Init() {
    shader_.Load("shaders/particle.vert", "shaders/particle.frag");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    const GLsizei stride = sizeof(Vertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, x));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, r));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, s));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    glEnable(GL_PROGRAM_POINT_SIZE);
}

void ParticleSystem::Shutdown() {
    if (vbo_) { glDeleteBuffers(1, &vbo_);      vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    emitters_.clear();
}

// ---------------------------------------------------------------------------
// SpawnEmitter
// ---------------------------------------------------------------------------

void ParticleSystem::SpawnEmitter(EmitterType type, glm::vec3 pos, float now, float duration) {
    Emitter e;
    e.type      = type;
    e.pos       = pos;
    e.startTime = now;
    e.duration  = duration;
    e.nextSpawn = now;

    const auto& cfg = kCfg[static_cast<int>(type)];
    if (cfg.burstCount > 0) {
        for (int i = 0; i < cfg.burstCount; ++i)
            spawnParticle(e, now);
        e.nextSpawn = 1e30f; // no further streaming
    }

    emitters_.push_back(std::move(e));
}

// ---------------------------------------------------------------------------
// spawnParticle
// ---------------------------------------------------------------------------

void ParticleSystem::spawnParticle(Emitter& e, float now) {
    const auto& cfg = kCfg[static_cast<int>(e.type)];

    glm::vec3 dir   = glm::normalize(cfg.velBias + glm::ballRand(cfg.spread));
    float     speed = cfg.speedMin + frand() * (cfg.speedMax - cfg.speedMin);

    Particle p;
    p.pos        = e.pos + glm::vec3((frand()-0.5f)*0.3f, 0.f, (frand()-0.5f)*0.3f);
    p.vel        = dir * speed;
    p.colorStart = cfg.colorStart;
    p.colorEnd   = cfg.colorEnd;
    p.sizeStart  = cfg.sizeStart;
    p.sizeEnd    = cfg.sizeEnd;
    p.age        = 0.f;
    p.lifetime   = cfg.lifetime * (0.7f + 0.6f * frand());

    e.particles.push_back(p);
}

float ParticleSystem::spawnInterval(EmitterType t) const {
    return kCfg[static_cast<int>(t)].streamInterval;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void ParticleSystem::Update(float now, float dt) {
    constexpr float kGravity = -4.5f;

    for (auto it = emitters_.begin(); it != emitters_.end(); ) {
        auto& e = *it;

        float age          = now - e.startTime;
        bool  emitterAlive = e.duration > 0.f && age < e.duration;
        float interval     = spawnInterval(e.type);

        if (emitterAlive && interval > 0.f) {
            while (e.nextSpawn <= now) {
                spawnParticle(e, e.nextSpawn);
                e.nextSpawn += interval;
            }
        }

        for (auto& p : e.particles) {
            p.age   += dt;
            p.vel.y += kGravity * dt;
            p.pos   += p.vel * dt;
        }

        e.particles.erase(
            std::remove_if(e.particles.begin(), e.particles.end(),
                [](const Particle& p){ return p.age >= p.lifetime; }),
            e.particles.end());

        if (!emitterAlive && e.particles.empty())
            it = emitters_.erase(it);
        else
            ++it;
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void ParticleSystem::Render(const glm::mat4& view, const glm::mat4& proj) {
    if (emitters_.empty() || shader_.id() == 0) return;

    verts_.clear();
    for (const auto& e : emitters_) {
        for (const auto& p : e.particles) {
            float     t   = p.age / p.lifetime;
            glm::vec4 col = glm::mix(p.colorStart, p.colorEnd, t);
            float     sz  = glm::mix(p.sizeStart,  p.sizeEnd,  t);
            verts_.push_back({p.pos.x, p.pos.y, p.pos.z,
                              col.r, col.g, col.b, col.a, sz});
        }
    }
    if (verts_.empty()) return;

    shader_.Use();
    shader_.SetMat4("uView", view);
    shader_.SetMat4("uProj", proj);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizei>(verts_.size() * sizeof(Vertex)),
                 verts_.data(), GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive blending for glow effect
    glDepthMask(GL_FALSE);

    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(verts_.size()));

    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
}

} // namespace rco::renderer
