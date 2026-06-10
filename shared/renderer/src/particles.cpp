#include "rco/renderer/particles.h"

#include "rco/renderer/shader.h"
#include <glm/gtc/random.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cstdlib>

namespace rco::renderer {

// ---------------------------------------------------------------------------
namespace {

static float frand() { return static_cast<float>(rand()) / RAND_MAX; }

} // namespace

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------

void ParticleSystem::Init() {
    // Shader compiled once by the engine via CompileAllShaders(); we just reference it.
    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);

    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));

    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, x));
    glVertexArrayAttribBinding(vao_, 0, 0);
    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, r));
    glVertexArrayAttribBinding(vao_, 1, 0);
    glEnableVertexArrayAttrib(vao_, 2);
    glVertexArrayAttribFormat(vao_, 2, 1, GL_FLOAT, GL_FALSE, offsetof(Vertex, s));
    glVertexArrayAttribBinding(vao_, 2, 0);

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

void ParticleSystem::SpawnEmitterParams(const FXParams& params,
                                       glm::vec3 pos,
                                       float now,
                                       float duration) {
    Emitter e;
    e.params    = params;
    e.pos       = pos;
    e.startTime = now;
    e.duration  = duration;
    e.nextSpawn = now;

    if (e.params.burstCount > 0) {
        for (int i = 0; i < e.params.burstCount; ++i)
            spawnParticle(e, now);
        e.nextSpawn = 1e30f; // no further streaming
    }

    emitters_.push_back(std::move(e));
}

// ---------------------------------------------------------------------------
// spawnParticle
// ---------------------------------------------------------------------------

void ParticleSystem::spawnParticle(Emitter& e, float now) {
    const auto& cfg = e.params;

    glm::vec3 dir = glm::normalize(cfg.velBias + glm::ballRand(cfg.velSpread));
    float speed = cfg.speedMin + frand() * (cfg.speedMax - cfg.speedMin);

    Particle p;
    p.pos        = e.pos + glm::vec3((frand()-0.5f)*0.3f, 0.f, (frand()-0.5f)*0.3f);
    p.vel        = dir * speed;
    p.colorStart = cfg.colorStart;
    p.colorEnd   = cfg.colorEnd;
    p.sizeStart  = cfg.sizeStart;
    p.sizeEnd    = cfg.sizeEnd;
    p.age        = 0.f;
    p.lifetime   = cfg.lifetimeSeconds * (0.7f + 0.6f * frand());

    e.particles.push_back(p);
}

float ParticleSystem::spawnInterval(const Emitter& e) const {
    return e.params.streamInterval;
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
        float interval     = spawnInterval(e);

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
    if (emitters_.empty()) return;

    auto it = Shader::shaders.find("particle");
    if (it == Shader::shaders.end() || !it->second) return;
    auto& shader = *it->second;

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

    shader.Bind();
    shader.SetMat4("uView", view);
    shader.SetMat4("uProj", proj);

    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);

    glNamedBufferData(vbo_,
                     static_cast<GLsizei>(verts_.size() * sizeof(Vertex)),
                     verts_.data(), GL_DYNAMIC_DRAW);

    glBindVertexArray(vao_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive blending for glow effect
    glDepthMask(GL_FALSE);

    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(verts_.size()));

    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(static_cast<GLuint>(prevVao));
}

} // namespace rco::renderer
