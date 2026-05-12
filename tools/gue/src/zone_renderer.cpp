#include "zone_renderer.h"
#include "zone_camera.h"
#include "zone_scene.h"
#include "asset_path.h"

#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fstream>

namespace gue {

// ─── Helpers ─────────────────────────────────────────────────────────────────

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, 512, nullptr, log);
        std::fprintf(stderr, "[ZoneRenderer] shader error: %s\n", log);
    }
    return sh;
}

static GLuint LinkProg(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, nullptr, log);
        std::fprintf(stderr, "[ZoneRenderer] link error: %s\n", log);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// ─── FBO ─────────────────────────────────────────────────────────────────────

ZoneRenderer::~ZoneRenderer() {
    DestroyFBO();
    // Actors destroy their models + bone SSBOs in their own destructors.
    sceneryActors_.clear();
    npcActors_.clear();
    if (primProg_)         glDeleteProgram(primProg_);
    if (colVisColorProg_)  glDeleteProgram(colVisColorProg_);
    if (sphereVAO_)   { glDeleteVertexArrays(1, &sphereVAO_); glDeleteBuffers(1, &sphereVBO_); glDeleteBuffers(1, &sphereEBO_); }
    if (boxVAO_)      { glDeleteVertexArrays(1, &boxVAO_);    glDeleteBuffers(1, &boxVBO_);    glDeleteBuffers(1, &boxEBO_);    }
    if (lineVAO_)     { glDeleteVertexArrays(1, &lineVAO_);   glDeleteBuffers(1, &lineVBO_);   }
    if (colVisBatchVAO_) { glDeleteVertexArrays(1, &colVisBatchVAO_); glDeleteBuffers(1, &colVisBatchVBO_); }
    // EditableTerrain cleans itself up via its destructor.
}

void ZoneRenderer::BuildFBO(int w, int h) {
    DestroyFBO();
    w_ = w; h_ = h;

    glCreateFramebuffers(1, &fbo_);

    glCreateTextures(GL_TEXTURE_2D, 1, &colorTex_);
    glTextureStorage2D(colorTex_, 1, GL_RGBA8, w, h);
    glTextureParameteri(colorTex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(colorTex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0, colorTex_, 0);

    glCreateRenderbuffers(1, &depthRbo_);
    glNamedRenderbufferStorage(depthRbo_, GL_DEPTH24_STENCIL8, w, h);
    glNamedFramebufferRenderbuffer(fbo_, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRbo_);
}

void ZoneRenderer::DestroyFBO() {
    if (colorTex_) { glDeleteTextures(1, &colorTex_);      colorTex_ = 0; }
    if (depthRbo_) { glDeleteRenderbuffers(1, &depthRbo_); depthRbo_ = 0; }
    if (fbo_)      { glDeleteFramebuffers(1, &fbo_);       fbo_      = 0; }
}

void ZoneRenderer::Resize(int w, int h) {
    if (w == w_ && h == h_) return;
    BuildFBO(w, h);
}

// ─── Primitive shader ─────────────────────────────────────────────────────────

static const char* kPrimVS = R"glsl(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 0) uniform mat4 u_VP;
layout(location = 1) uniform mat4 u_M;
void main() {
    gl_Position = u_VP * u_M * vec4(aPos, 1.0);
}
)glsl";

static const char* kPrimFS = R"glsl(
#version 460 core
layout(location = 2) uniform vec4 u_Color;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = u_Color;
}
)glsl";

// Per-vertex colour shader — used for the batched colVis geometry.
// Vertex layout: vec3 pos + vec4 col (7 floats, stride 28 bytes).
static const char* kColVisVS = R"glsl(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 0) uniform mat4 u_VP;
out vec4 vCol;
void main() {
    gl_Position = u_VP * vec4(aPos, 1.0);
    vCol = aCol;
}
)glsl";

static const char* kColVisFS = R"glsl(
#version 460 core
in  vec4 vCol;
out vec4 fragColor;
void main() { fragColor = vCol; }
)glsl";

bool ZoneRenderer::InitPrimShader() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER,   kPrimVS);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kPrimFS);
    primProg_ = LinkProg(vs, fs);

    GLuint cvs = CompileShader(GL_VERTEX_SHADER,   kColVisVS);
    GLuint cfs = CompileShader(GL_FRAGMENT_SHADER, kColVisFS);
    colVisColorProg_ = LinkProg(cvs, cfs);

    return primProg_ != 0 && colVisColorProg_ != 0;
}

// ─── Scene actor cache ───────────────────────────────────────────────────────
// We no longer maintain a custom mesh shader: every scenery prop and NPC is
// drawn through rco::renderer::Actor + the deferred pipeline (same path as
// the Media preview and the game client). "Simple" mode is just the same
// pipeline with atmospheric effects disabled via FeatureConfig.

// ResolveClientAsset is shared via asset_path.h.

void ZoneRenderer::SyncSceneryModels(const std::vector<ZScenery>& scenery,
                                      const std::unordered_map<int, ModelBind>& binds) {
    // Actors require the full engine + pipeline. If the caller hasn't invoked
    // Init→EnsurePBR_ yet, defer.
    if (!fullEngine_) return;
    auto& mm = fullEngine_->materials();

    // Evict actors whose scenery entries no longer exist in the scene.
    std::unordered_set<int> in_scene;
    for (auto& s : scenery) in_scene.insert(s.id);
    for (auto it = sceneryActors_.begin(); it != sceneryActors_.end(); ) {
        if (!in_scene.count(it->first)) {
            int erased_id = it->first;
            it = sceneryActors_.erase(it);
            sceneryActorModelId_.erase(erased_id);
        } else {
            ++it;
        }
    }

    bool need_rebuild = false;
    for (const auto& s : scenery) {
        auto bit = binds.find(s.modelId);
        if (bit == binds.end() || bit->second.file_path.empty()) continue;

        int prev_mid = -1;
        auto midIt = sceneryActorModelId_.find(s.id);
        if (midIt != sceneryActorModelId_.end()) prev_mid = midIt->second;

        if (prev_mid != s.modelId) {
            // Fresh or rebind: (re)create the actor from the bound model.
            auto a = std::make_unique<rco::renderer::Actor>();
            std::string resolved = ResolveClientAsset(bit->second.file_path);
            a->Init("", resolved.c_str(), &mm);
            if (a->IsLoaded() && !bit->second.material_map.empty())
                a->ApplyMaterialsByName(mm, bit->second.material_map);
            sceneryActors_[s.id] = std::move(a);
            sceneryActorModelId_[s.id] = s.modelId;
            need_rebuild = true;
        } else {
            // Same model — just re-apply mapping in case it changed in Media.
            auto& a = sceneryActors_[s.id];
            if (a && a->IsLoaded() && !bit->second.material_map.empty()) {
                a->ApplyMaterialsByName(mm, bit->second.material_map);
                need_rebuild = true;
            }
        }
    }

    if (need_rebuild) fullEngine_->RebuildMaterialsBuffer();
}

void ZoneRenderer::SetGhostModel(int modelId, const std::string& filePath,
                                  const glm::vec3& pos, float yaw, float scale) {
    if (modelId < 0) {
        ghostActor_.reset();
        ghostModelId_ = -1;
        return;
    }

    // Reload actor only when the model changes.
    if (modelId != ghostModelId_) {
        ghostActor_ = std::make_unique<rco::renderer::Actor>();
        std::string resolved = "../client/" + filePath;
        // Use existing model cache — no extra disk I/O.
        rco::renderer::MaterialManager* mm =
            fullEngine_ ? &fullEngine_->materials() : nullptr;
        ghostActor_->Init("", resolved.c_str(), mm);
        if (fullEngine_) fullEngine_->RebuildMaterialsBuffer();
        ghostModelId_ = modelId;
    }

    // Build transform matrix (no pitch/roll for placement).
    glm::mat4 m = glm::translate(glm::mat4(1.f), pos);
    m = glm::rotate(m, glm::radians(yaw), {0.f, 1.f, 0.f});
    m = glm::scale(m, glm::vec3(scale));
    ghostTransform_ = m;
    ghostPos_ = pos;
}

void ZoneRenderer::SyncNpcModels(const std::unordered_map<int, ModelBind>& npcBinds) {
    if (!fullEngine_) return;
    auto& mm = fullEngine_->materials();

    // Evict NPC actors that are no longer in the scene OR whose bind path
    // went empty (ActorDef cleared). Also evict their scale entries.
    for (auto it = npcActors_.begin(); it != npcActors_.end(); ) {
        auto bit = npcBinds.find(it->first);
        if (bit == npcBinds.end() || bit->second.file_path.empty()) {
            npcActorScale_.erase(it->first);
            it = npcActors_.erase(it);
        } else {
            ++it;
        }
    }

    bool need_rebuild = false;
    for (const auto& [id, bind] : npcBinds) {
        if (bind.file_path.empty()) continue;

        // Cache actor scale — refreshed every sync so edits to ActorDef.scale
        // in the Media tab propagate the next time zones resyncs (on selection
        // change or MediaRevision bump).
        npcActorScale_[id] = bind.actor_scale > 0.f ? bind.actor_scale : 1.f;

        auto it = npcActors_.find(id);
        rco::renderer::Actor* actor = nullptr;
        bool created = false;
        if (it == npcActors_.end()) {
            // New NPC in the scene — create a fresh actor.
            auto a = std::make_unique<rco::renderer::Actor>();
            std::string resolved = ResolveClientAsset(bind.file_path);
            a->Init("", resolved.c_str(), &mm);
            actor = a.get();
            npcActors_[id] = std::move(a);
            created = true;
        } else {
            actor = it->second.get();
        }

        if (actor && actor->IsLoaded()) {
            // Apply per-aiMaterial mapping first (Substance-style names →
            // media_materials), then the Actor Def's per-slot global override
            // on top — matches the Media preview path so textures look the
            // same in both viewports.
            if (!bind.material_map.empty()) {
                actor->ApplyMaterialsByName(mm, bind.material_map);
                need_rebuild = true;
            }
            if (bind.has_override) {
                actor->OverrideMaterial(
                    ResolveClientAsset(bind.material_override.albedo),
                    ResolveClientAsset(bind.material_override.normal),
                    ResolveClientAsset(bind.material_override.orm),
                    bind.ovr_albedo_r, bind.ovr_albedo_g, bind.ovr_albedo_b,
                    bind.ovr_roughness, bind.ovr_metallic);
                need_rebuild = true;
            }
            (void)created;
        }
    }

    if (need_rebuild) fullEngine_->RebuildMaterialsBuffer();
}

// ─── Primitive VAOs ───────────────────────────────────────────────────────────

static void GenSphere(std::vector<float>& verts, std::vector<uint16_t>& idx,
                      int stacks = 12, int slices = 16) {
    for (int i = 0; i <= stacks; ++i) {
        float phi = (float)i / stacks * glm::pi<float>();
        for (int j = 0; j <= slices; ++j) {
            float theta = (float)j / slices * 2.f * glm::pi<float>();
            float x = std::sin(phi) * std::cos(theta);
            float y = std::cos(phi);
            float z = std::sin(phi) * std::sin(theta);
            verts.push_back(x); verts.push_back(y); verts.push_back(z);
        }
    }
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            int a = i * (slices+1) + j;
            idx.push_back((uint16_t)a);
            idx.push_back((uint16_t)(a + slices + 1));
            idx.push_back((uint16_t)(a + 1));
            idx.push_back((uint16_t)(a + 1));
            idx.push_back((uint16_t)(a + slices + 1));
            idx.push_back((uint16_t)(a + slices + 2));
        }
    }
}

static const float kBoxVerts[] = {
    // 8 corners
    -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f, -0.5f, 0.5f,-0.5f,
    -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
};
static const uint16_t kBoxIdx[] = {
    0,1,2, 2,3,0,  4,5,6, 6,7,4,
    0,1,5, 5,4,0,  2,3,7, 7,6,2,
    0,3,7, 7,4,0,  1,2,6, 6,5,1,
};

void ZoneRenderer::BuildPrimitiveVAOs() {
    // Sphere
    std::vector<float>    sv; std::vector<uint16_t> si;
    GenSphere(sv, si);
    sphereIdx_ = (int)si.size();
    glCreateVertexArrays(1, &sphereVAO_);
    glCreateBuffers(1, &sphereVBO_); glCreateBuffers(1, &sphereEBO_);
    glNamedBufferData(sphereVBO_, sv.size()*4, sv.data(), GL_STATIC_DRAW);
    glNamedBufferData(sphereEBO_, si.size()*2, si.data(), GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(sphereVAO_, 0, sphereVBO_, 0, 12);
    glVertexArrayElementBuffer(sphereVAO_, sphereEBO_);
    glEnableVertexArrayAttrib(sphereVAO_, 0);
    glVertexArrayAttribFormat(sphereVAO_, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(sphereVAO_, 0, 0);

    // Box
    boxIdx_ = sizeof(kBoxIdx) / sizeof(uint16_t);
    glCreateVertexArrays(1, &boxVAO_);
    glCreateBuffers(1, &boxVBO_); glCreateBuffers(1, &boxEBO_);
    glNamedBufferData(boxVBO_, sizeof(kBoxVerts), kBoxVerts, GL_STATIC_DRAW);
    glNamedBufferData(boxEBO_, sizeof(kBoxIdx),   kBoxIdx,   GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(boxVAO_, 0, boxVBO_, 0, 12);
    glVertexArrayElementBuffer(boxVAO_, boxEBO_);
    glEnableVertexArrayAttrib(boxVAO_, 0);
    glVertexArrayAttribFormat(boxVAO_, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(boxVAO_, 0, 0);

    // Line (dynamic — 2 verts updated per call)
    glCreateVertexArrays(1, &lineVAO_);
    glCreateBuffers(1, &lineVBO_);
    glNamedBufferData(lineVBO_, 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexArrayVertexBuffer(lineVAO_, 0, lineVBO_, 0, 12);
    glEnableVertexArrayAttrib(lineVAO_, 0);
    glVertexArrayAttribFormat(lineVAO_, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(lineVAO_, 0, 0);

    // ColVis batch — per-vertex colour, dynamic (rebuilt when scene changes)
    // Stride: 7 × 4 = 28 bytes  [pos.xyz | col.rgba]
    glCreateVertexArrays(1, &colVisBatchVAO_);
    glCreateBuffers(1, &colVisBatchVBO_);
    glNamedBufferData(colVisBatchVBO_, 0, nullptr, GL_DYNAMIC_DRAW);
    glVertexArrayVertexBuffer(colVisBatchVAO_, 0, colVisBatchVBO_, 0, 28);
    // attrib 0 = pos (offset 0)
    glEnableVertexArrayAttrib(colVisBatchVAO_, 0);
    glVertexArrayAttribFormat(colVisBatchVAO_, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(colVisBatchVAO_, 0, 0);
    // attrib 1 = colour (offset 12)
    glEnableVertexArrayAttrib(colVisBatchVAO_, 1);
    glVertexArrayAttribFormat(colVisBatchVAO_, 1, 4, GL_FLOAT, GL_FALSE, 12);
    glVertexArrayAttribBinding(colVisBatchVAO_, 1, 0);
}

// ─── Init ─────────────────────────────────────────────────────────────────────

bool ZoneRenderer::Init() {
    if (fbo_) return true;  // already initialised
    BuildFBO(800, 600);
    primsReady_ = InitPrimShader() && (BuildPrimitiveVAOs(), true);
    // Eagerly create the deferred engine + pipeline so scenery and NPC
    // actors can live as soon as the Zones tab opens. This pays ~1s of
    // shader compile at startup, but from then on Simple and Lit share
    // the same code path.
    EnsurePBR_(800, 600);
    return fbo_ != 0;
}

// ─── Terrain loading — delegates to EditableTerrain ──────────────────────────

void ZoneRenderer::LoadTerrain(const std::string& areaName) {
    if (areaName == loadedArea_) return;
    loadedArea_ = areaName;
    terrain_.LoadArea(areaName);
}

// ─── Draw helpers ─────────────────────────────────────────────────────────────

void ZoneRenderer::DrawSphere(const glm::vec3& pos, float r,
                               const glm::vec4& col, const glm::mat4& vp) {
    glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(vp));
    glm::mat4 m = glm::scale(glm::translate(glm::mat4(1.f), pos), glm::vec3(r));
    glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m));
    glUniform4fv(2, 1, glm::value_ptr(col));
    glBindVertexArray(sphereVAO_);
    glDrawElements(GL_TRIANGLES, sphereIdx_, GL_UNSIGNED_SHORT, nullptr);
}

void ZoneRenderer::DrawBox(const glm::vec3& pos, const glm::vec3& scale,
                            const glm::vec4& col, const glm::mat4& vp) {
    glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(vp));
    glm::mat4 m = glm::scale(glm::translate(glm::mat4(1.f), pos), scale);
    glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m));
    glUniform4fv(2, 1, glm::value_ptr(col));
    glBindVertexArray(boxVAO_);
    glDrawElements(GL_TRIANGLES, boxIdx_, GL_UNSIGNED_SHORT, nullptr);
}

void ZoneRenderer::DrawLine(const glm::vec3& a, const glm::vec3& b,
                             const glm::vec4& col, const glm::mat4& vp) {
    float verts[6] = {a.x, a.y, a.z, b.x, b.y, b.z};
    glNamedBufferSubData(lineVBO_, 0, sizeof(verts), verts);
    glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(vp));
    glm::mat4 identity(1.f);
    glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(identity));
    glUniform4fv(2, 1, glm::value_ptr(col));
    glBindVertexArray(lineVAO_);
    glDrawArrays(GL_LINES, 0, 2);
}

void ZoneRenderer::UploadColVisBatch(const ColVisData& vis) {
    colVisBatchVtxN_ = (int)vis.verts.size();
    if (colVisBatchVtxN_ == 0) return;
    glNamedBufferData(colVisBatchVBO_,
                      colVisBatchVtxN_ * (GLsizeiptr)sizeof(ColVisData::Vtx),
                      vis.verts.data(),
                      GL_DYNAMIC_DRAW);
}

void ZoneRenderer::DrawCircleXZ(const glm::vec2& xz, float r, float y,
                                 const glm::vec4& col, const glm::mat4& vp) {
    const int segs = 32;
    float prev_x = xz.x + r, prev_z = xz.y;
    for (int i = 1; i <= segs; ++i) {
        float ang  = (float)i / segs * 2.f * glm::pi<float>();
        float nx   = xz.x + r * std::cos(ang);
        float nz   = xz.y + r * std::sin(ang);
        DrawLine({prev_x, y, prev_z}, {nx, y, nz}, col, vp);
        prev_x = nx; prev_z = nz;
    }
}

// ─── RenderFrame ──────────────────────────────────────────────────────────────

void ZoneRenderer::RenderFrame(const ZoneCamera& cam, const ZoneScene& scene,
                                int selectedID, int selectedType, float dt) {
    // Single render path — both Simple and Lit go through the deferred
    // pipeline. Simple is just a cheaper FeatureConfig (see RenderFramePBR_).
    RenderFramePBR_(cam, scene, selectedID, selectedType, dt);
}

ImTextureID ZoneRenderer::GetTexture() const {
    if (fullEngine_) return (ImTextureID)(intptr_t)fullEngine_->finalImage();
    return (ImTextureID)(intptr_t)colorTex_;
}

void ZoneRenderer::SetRenderMode(RenderMode m) {
    renderMode_ = m;
    // PBR engine is lazily created on first frame (EnsurePBR_).
}

void ZoneRenderer::EnsurePBR_(int w, int h) {
    if (!fullEngine_) {
        fullEngine_ = std::make_unique<rco::renderer::Engine>();
        rco::renderer::EngineConfig cfg{};
        cfg.width      = std::max(w, 64);
        cfg.height     = std::max(h, 64);
        cfg.shader_dir = "../client/shaders/";  // GUE lives in dist/tools/
        fullEngine_->Init(cfg);
        fullEngine_->LoadEnvironment("../client/assets/ibl/default.hdr");
        fullPipeline_ = std::make_unique<rco::renderer::Pipeline>(*fullEngine_);
        // Volumetric fog off by default — editor doesn't need atmospheric haze.
        rco::renderer::FeatureConfig pcfg;
        pcfg.volumetrics = false;
        fullPipeline_->SetFeatures(pcfg);
    } else if (fullEngine_->width() != w || fullEngine_->height() != h) {
        fullEngine_->Resize(std::max(w, 64), std::max(h, 64));
    }
}

void ZoneRenderer::RenderFramePBR_(const ZoneCamera& cam, const ZoneScene& scene,
                                    int selectedID, int selectedType, float dt) {
    EnsurePBR_(w_, h_);
    if (!fullEngine_ || !fullPipeline_) return;
    elapsed_time_ += dt;

    // Simple vs Lit — same pipeline, just different FeatureConfig. Simple
    // turns off the expensive post-effects (SSAO, volumetrics, SSR, FXAA);
    // PBR geometry + sun direct lighting + IBL still run, so models keep
    // their correct colour and texture.
    {
        rco::renderer::FeatureConfig fc;
        if (renderMode_ == kRenderSimple) {
            fc.ssao        = false;
            fc.volumetrics = false;
            fc.ssr         = false;
            fc.fxaa        = false;
        } else {
            fc.volumetrics = false;  // still off — editor, not game
        }
        fullPipeline_->SetFeatures(fc);
    }

    float aspect = (float)w_ / (float)h_;
    glm::mat4 view = cam.View();
    glm::mat4 proj = cam.Proj(aspect);
    last_cam_pos_ = cam.pos;

    fullPipeline_->Begin(view, proj, cam.pos, dt);
    fullPipeline_->SetSun(-glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f)),
                          glm::vec3(1.0f, 0.96f, 0.85f));

    terrain_.SubmitToPipeline(*fullPipeline_, cam.pos);

    // Scenery — one Actor per ZScenery id (see sceneryActors_). Each carries
    // its own bone SSBOs, material mapping, etc. — the same class Media uses.
    for (const auto& s : scene.scenery) {
        auto it = sceneryActors_.find(s.id);
        if (it == sceneryActors_.end() || !it->second || !it->second->IsLoaded()) continue;
        glm::mat4 m = glm::translate(glm::mat4(1.f), s.pos);
        m = glm::rotate(m, glm::radians(s.rot.y), {0.f, 1.f, 0.f});
        m = glm::rotate(m, glm::radians(s.rot.x), {1.f, 0.f, 0.f});
        m = glm::rotate(m, glm::radians(s.rot.z), {0.f, 0.f, 1.f});
        m = glm::scale(m, s.scale);
        it->second->SubmitWithMatrix(*fullPipeline_, m);
    }

    // NPCs — Actor::SubmitAs drives skinning so the idle clip plays in the
    // editor viewport (same behaviour as Media preview). Actor-level scale
    // comes from the ActorDef via npcActorScale_ (synced in SyncNpcModels).
    for (const auto& n : scene.npcs) {
        auto it = npcActors_.find(n.id);
        if (it == npcActors_.end() || !it->second || !it->second->IsLoaded()) continue;
        auto sit = npcActorScale_.find(n.id);
        const float s = (sit != npcActorScale_.end()) ? sit->second : 1.f;
        it->second->position = n.pos;
        it->second->yaw      = n.yaw;
        it->second->scale    = s;
        const std::string& anim = it->second->CurrentAnim().empty()
            ? std::string("Idle")
            : it->second->CurrentAnim();
        it->second->SubmitAs(anim, elapsed_time_, /*loop*/true, *fullPipeline_);
    }

    // Ghost: drag-and-drop preview rendered into the deferred pipeline so it
    // receives correct depth sorting against the scene.
    if (ghostModelId_ >= 0 && ghostActor_ && ghostActor_->IsLoaded()) {
        ghostActor_->SubmitWithMatrix(*fullPipeline_, ghostTransform_);
    }

    // Forward pass — overlays (portals, triggers, colboxes, gizmo) into the
    // same FBO so they respect the deferred depth buffer.
    glm::mat4 vp = proj * view;
    fullPipeline_->End([&]() {
        DrawForwardOverlays_(scene, selectedID, selectedType, vp);

        // Ghost outline — drawn on top with additive blue tint so the user
        // can see it's a preview. Uses the prim shader in wireframe mode.
        if (ghostModelId_ >= 0 && ghostActor_ && ghostActor_->IsLoaded()) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glDisable(GL_CULL_FACE);
            glLineWidth(1.5f);
            // Reuse primProg_ with a blue-white tint applied via u_col.
            if (primProg_) {
                glUseProgram(primProg_);
                glUniformMatrix4fv(glGetUniformLocation(primProg_, "u_mvp"),
                    1, GL_FALSE, glm::value_ptr(vp * ghostTransform_));
                glUniform4f(glGetUniformLocation(primProg_, "u_col"),
                    0.4f, 0.8f, 1.0f, 0.9f);
                for (const auto& m : ghostActor_->model().meshes()) {
                    if (!m.vao || m.idx_count == 0) continue;
                    glBindVertexArray(m.vao);
                    glDrawElements(GL_TRIANGLES, m.idx_count,
                                   GL_UNSIGNED_INT, nullptr);
                }
                glBindVertexArray(0);
            }
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glEnable(GL_CULL_FACE);
            glLineWidth(1.f);
        }
    });
}

void ZoneRenderer::DrawForwardOverlays_(const ZoneScene& scene, int selectedID,
                                         int selectedType, const glm::mat4& vp) {
    if (!primsReady_) return;
    glUseProgram(primProg_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Portals / triggers / sound zones / colboxes / water / emitters / waypoints
    // — same overlays as the simple renderer. NPC meshes are NOT drawn here
    // because they went through the G-buffer path above.
    for (auto& p : scene.portals) {
        bool sel = (selectedType == kSelPortal && selectedID == p.id);
        glm::vec4 col = sel ? glm::vec4(1.f, 0.2f, 0.2f, 0.7f)
                            : glm::vec4(0.2f, 0.4f, 1.0f, 0.5f);
        DrawSphere({p.pos.x, p.pos.y + p.radius, p.pos.z}, p.radius, col, vp);
    }
    for (auto& t : scene.triggers) {
        bool sel = (selectedType == kSelTrigger && selectedID == t.id);
        glm::vec4 col = sel ? glm::vec4(1.f, 0.2f, 0.2f, 0.7f)
                            : glm::vec4(1.0f, 0.5f, 0.0f, 0.4f);
        DrawSphere({t.x, 0.f, t.z}, t.radius, col, vp);
    }
    for (auto& s : scene.soundZones) {
        bool sel = (selectedType == kSelSoundZone && selectedID == s.id);
        glm::vec4 col = sel ? glm::vec4(1.f, 0.2f, 0.2f, 0.7f)
                            : glm::vec4(1.0f, 1.0f, 0.0f, 0.4f);
        DrawSphere({s.x, 0.f, s.z}, s.radius, col, vp);
    }
    for (auto& c : scene.colBoxes) {
        bool sel = (selectedType == kSelColBox && selectedID == c.id);
        glm::vec4 col = sel ? glm::vec4(1.f, 0.2f, 0.2f, 0.8f)
                            : glm::vec4(0.8f, 0.1f, 0.1f, 0.4f);
        DrawBox(c.pos, c.scale, col, vp);
    }
    for (auto& s : scene.colSpheres) {
        bool sel = (selectedType == kSelColSphere && selectedID == s.id);
        glm::vec4 col = sel ? glm::vec4(1.f, 0.2f, 0.2f, 0.8f)
                            : glm::vec4(1.0f, 0.4f, 0.0f, 0.45f);
        DrawSphere(s.pos, s.radius, col, vp);
    }
    // Per-scenery collision shapes — one batched draw call for all shapes.
    if (colVisBatchVtxN_ > 0 && colVisColorProg_) {
        glUseProgram(colVisColorProg_);
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(vp));
        glBindVertexArray(colVisBatchVAO_);
        glDrawArrays(GL_LINES, 0, colVisBatchVtxN_);
        glUseProgram(primProg_);  // restore for subsequent draw calls
    }
    for (auto& w : scene.water) {
        bool sel = (selectedType == kSelWater && selectedID == w.id);
        float a  = sel ? 0.8f : w.opacity / 100.f;
        glm::vec4 col = {w.color.r, w.color.g, w.color.b, a};
        DrawBox(w.pos, {w.scale.x, 0.2f, w.scale.y}, col, vp);
    }
    for (auto& e : scene.emitters) {
        bool sel = (selectedType == kSelEmitter && selectedID == e.id);
        glm::vec4 col = sel ? glm::vec4(1.f, 0.2f, 0.2f, 0.7f)
                            : glm::vec4(0.8f, 1.0f, 0.2f, 0.6f);
        DrawBox(e.pos, {0.8f, 1.5f, 0.8f}, col, vp);
    }
    static const glm::vec4 kNpcAggColors[] = {
        {0.1f, 0.9f, 0.1f, 0.35f}, {1.0f, 0.9f, 0.0f, 0.35f},
        {1.0f, 0.2f, 0.1f, 0.35f}, {0.7f, 0.3f, 1.0f, 0.35f},
    };
    for (auto& n : scene.npcs) {
        bool sel = (selectedType == kSelNpc && selectedID == n.id);
        int ci = std::clamp(n.aggressiveness, 0, 3);
        if (n.aggressiveness == 1 || n.aggressiveness == 2)
            DrawCircleXZ({n.pos.x, n.pos.z}, n.aggroRange, n.pos.y + 0.1f,
                         kNpcAggColors[ci], vp);
        if (sel)
            DrawCircleXZ({n.pos.x, n.pos.z}, n.attackRange, n.pos.y + 0.1f,
                         {1.f, 1.f, 1.f, 0.35f}, vp);
    }
    for (auto& sp : scene.spawnPoints) {
        bool sel = (selectedType == kSelSpawnPoint && selectedID == sp.id);
        glm::vec4 colSphere = sel ? glm::vec4(1.f, 0.9f, 0.1f, 0.9f)
                                  : glm::vec4(0.1f, 0.9f, 0.2f, 0.7f);
        glm::vec4 colCircle = sel ? glm::vec4(1.f, 0.9f, 0.1f, 0.6f)
                                  : glm::vec4(0.1f, 0.9f, 0.2f, 0.4f);
        DrawSphere(sp.pos, 0.5f, colSphere, vp);
        DrawCircleXZ({sp.pos.x, sp.pos.z}, sp.radius, sp.pos.y + 0.1f, colCircle, vp);
    }
    for (auto& w : scene.waypoints) {
        bool sel = (selectedType == kSelWaypoint && selectedID == w.id);
        glm::vec4 col = sel ? glm::vec4(1.f, 0.2f, 0.2f, 0.9f)
                            : glm::vec4(0.0f, 0.8f, 1.0f, 0.9f);
        DrawBox(w.pos, {0.8f, 0.8f, 0.8f}, col, vp);
    }
    for (auto& w : scene.waypoints) {
        auto findPos = [&](int id) -> glm::vec3 {
            for (auto& o : scene.waypoints) if (o.id == id) return o.pos;
            return w.pos;
        };
        if (w.nextA >= 0)
            DrawLine(w.pos + glm::vec3(0,0.4f,0), findPos(w.nextA) + glm::vec3(0,0.4f,0),
                     {0.0f, 0.4f, 1.0f, 0.8f}, vp);
        if (w.nextB >= 0)
            DrawLine(w.pos + glm::vec3(0,0.4f,0), findPos(w.nextB) + glm::vec3(0,0.4f,0),
                     {1.0f, 0.5f, 0.0f, 0.8f}, vp);
    }

    glDisable(GL_BLEND);

    // ── Gizmo (set by the tab via SetGizmo before RenderFrame) ───────────
    // Drawn on top of overlays, still inside the pipeline's forward pass so
    // it lands on the same FBO as the rendered scene.
    if (gizmo_.mode != kGizmoNone) {
        // Recover cam pos from the inverse-view in vp by reconstructing a
        // reasonable approximation: pull it from a stored member instead.
        // Simpler: the vp carries everything we need for the draw; axis
        // length uses vp itself to scale. We keep the existing helper that
        // expects camPos, so read the member set by RenderFramePBR_.
        glm::vec3 cp = last_cam_pos_;
        switch (gizmo_.mode) {
        case kGizmoMove:
            DrawMoveGizmo(gizmo_.pos, vp, cp, gizmo_.axis);
            break;
        case kGizmoRotate:
            DrawRotateGizmo(gizmo_.pos, vp, cp, gizmo_.axis, gizmo_.allow_axes);
            break;
        case kGizmoScale:
            DrawScaleGizmo(gizmo_.pos, vp, cp, gizmo_.axis, gizmo_.allow_axes);
            break;
        default: break;
        }
    }
}


// ─── Move gizmo ──────────────────────────────────────────────────────────────

float ZoneRenderer::GizmoAxisLength(const glm::vec3& objPos, const glm::vec3& camPos) {
    // Scale the gizmo with distance from camera so it stays a roughly-constant
    // screen size. ~9% of distance, clamped so it's neither tiny nor huge.
    float d = glm::length(objPos - camPos);
    return std::clamp(d * 0.09f, 1.2f, 40.f);
}

void ZoneRenderer::DrawMoveGizmo(const glm::vec3& pos, const glm::mat4& vp,
                                 const glm::vec3& camPos, int highlightAxis) {
    // Caller (pipeline forward-pass callback) controls the FBO binding; we
    // just draw into whatever target is currently bound.
    if (!primsReady_) return;
    glUseProgram(primProg_);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float len = GizmoAxisLength(pos, camPos);
    float thick = len * 0.05f;
    float head  = len * 0.15f;

    struct Axis { glm::vec3 dir; glm::vec4 col; };
    Axis axes[3] = {
        {{1,0,0}, {1.00f, 0.25f, 0.25f, 1.f}},
        {{0,1,0}, {0.25f, 1.00f, 0.25f, 1.f}},
        {{0,0,1}, {0.25f, 0.40f, 1.00f, 1.f}},
    };

    for (int i = 0; i < 3; ++i) {
        glm::vec4 col = axes[i].col;
        if (highlightAxis == i) col = glm::vec4(1.f, 0.95f, 0.2f, 1.f);
        glm::vec3 mid   = pos + axes[i].dir * (len * 0.5f);
        glm::vec3 scale = glm::vec3(thick);
        if (i == 0) scale.x = len;
        if (i == 1) scale.y = len;
        if (i == 2) scale.z = len;
        DrawBox(mid, scale, col, vp);
        glm::vec3 tip = pos + axes[i].dir * len;
        DrawBox(tip, glm::vec3(head), col, vp);
    }
    DrawBox(pos, glm::vec3(thick * 1.5f), {0.9f, 0.9f, 0.9f, 1.f}, vp);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// ─── Rotate gizmo ────────────────────────────────────────────────────────────

void ZoneRenderer::DrawRotateGizmo(const glm::vec3& pos, const glm::mat4& vp,
                                    const glm::vec3& camPos, int highlightAxis,
                                    unsigned allowAxes) {
    if (!primsReady_) return;
    glUseProgram(primProg_);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const float radius = GizmoAxisLength(pos, camPos);
    const int   segments = 48;
    const glm::vec3 kAxes[3] = {{1,0,0},{0,1,0},{0,0,1}};
    const glm::vec4 kColors[3] = {
        {1.00f, 0.25f, 0.25f, 1.f},
        {0.25f, 1.00f, 0.25f, 1.f},
        {0.25f, 0.40f, 1.00f, 1.f},
    };

    for (int a = 0; a < 3; ++a) {
        if ((allowAxes & (1u << a)) == 0) continue;
        glm::vec4 col = (highlightAxis == a)
            ? glm::vec4(1.f, 0.95f, 0.2f, 1.f)
            : kColors[a];
        glm::vec3 u = (a == 1) ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
        glm::vec3 t = glm::normalize(glm::cross(kAxes[a], u));
        glm::vec3 b = glm::normalize(glm::cross(kAxes[a], t));
        glm::vec3 prev = pos + t * radius;
        for (int i = 1; i <= segments; ++i) {
            float ang = (float)i / segments * 2.f * glm::pi<float>();
            glm::vec3 cur = pos + t * (std::cos(ang) * radius)
                                + b * (std::sin(ang) * radius);
            DrawLine(prev, cur, col, vp);
            prev = cur;
        }
    }
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

// ─── Scale gizmo ─────────────────────────────────────────────────────────────

void ZoneRenderer::DrawScaleGizmo(const glm::vec3& pos, const glm::mat4& vp,
                                   const glm::vec3& camPos, int highlightAxis,
                                   unsigned allowAxes) {
    if (!primsReady_) return;
    glUseProgram(primProg_);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const float len   = GizmoAxisLength(pos, camPos);
    const float thick = len * 0.04f;
    const float cube  = len * 0.13f;

    struct Axis { glm::vec3 dir; glm::vec4 col; };
    Axis axes[3] = {
        {{1,0,0}, {1.00f, 0.25f, 0.25f, 1.f}},
        {{0,1,0}, {0.25f, 1.00f, 0.25f, 1.f}},
        {{0,0,1}, {0.25f, 0.40f, 1.00f, 1.f}},
    };
    for (int i = 0; i < 3; ++i) {
        if ((allowAxes & (1u << i)) == 0) continue;
        glm::vec4 col = axes[i].col;
        if (highlightAxis == i) col = glm::vec4(1.f, 0.95f, 0.2f, 1.f);
        glm::vec3 mid = pos + axes[i].dir * (len * 0.5f);
        glm::vec3 shaftScale(thick);
        if (i == 0) shaftScale.x = len;
        if (i == 1) shaftScale.y = len;
        if (i == 2) shaftScale.z = len;
        DrawBox(mid, shaftScale, col, vp);
        glm::vec3 tip = pos + axes[i].dir * len;
        DrawBox(tip, glm::vec3(cube), col, vp);
    }
    {
        glm::vec4 col = (highlightAxis == 3)
            ? glm::vec4(1.f, 0.95f, 0.2f, 1.f)
            : glm::vec4(0.85f, 0.85f, 0.85f, 1.f);
        DrawBox(pos, glm::vec3(cube * 0.9f), col, vp);
    }
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

} // namespace gue
