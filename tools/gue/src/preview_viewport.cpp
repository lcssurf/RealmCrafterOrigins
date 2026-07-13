#include "preview_viewport.h"
#include "asset_path.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"
#include "rco/renderer/shader.h"
#include "rco/renderer/model_cache.h"
#include "rco/renderer/col_bake.h"

namespace gue {

namespace {

struct QuantVtxKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

inline bool operator==(const QuantVtxKey& a, const QuantVtxKey& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

struct QuantVtxKeyLess {
    bool operator()(const QuantVtxKey& a, const QuantVtxKey& b) const {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    }
};

struct QuantVtxKeyHash {
    size_t operator()(const QuantVtxKey& v) const {
        size_t h = std::hash<int32_t>{}(v.x);
        h ^= std::hash<int32_t>{}(v.y) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(v.z) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct QuantTriKey {
    QuantVtxKey a;
    QuantVtxKey b;
    QuantVtxKey c;
};

inline bool operator==(const QuantTriKey& lhs, const QuantTriKey& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}

struct QuantTriKeyHash {
    size_t operator()(const QuantTriKey& t) const {
        QuantVtxKeyHash hv;
        size_t h = hv(t.a);
        h ^= hv(t.b) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= hv(t.c) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

size_t BuildQuantizedTriangles(
    const std::vector<std::array<glm::vec3, 3>>& src,
    int                                           gridDiv,
    float                                         areaCullFactor,
    std::vector<std::array<glm::vec3, 3>>*       out) {
    if (src.empty()) {
        if (out) out->clear();
        return 0;
    }

    gridDiv = std::max(1, gridDiv);

    glm::vec3 bmin = src[0][0];
    glm::vec3 bmax = src[0][0];
    for (const auto& tri : src) {
        for (const auto& p : tri) {
            bmin = glm::min(bmin, p);
            bmax = glm::max(bmax, p);
        }
    }
    glm::vec3 extent = bmax - bmin;
    float maxExtent = std::max(extent.x, std::max(extent.y, extent.z));
    if (maxExtent <= 1e-5f) {
        if (out) *out = src;
        return src.size();
    }

    float cell = maxExtent / (float)gridDiv;
    if (cell < 1e-6f) cell = 1e-6f;
    const float invCell = 1.f / cell;
    if (areaCullFactor < 1e-7f) areaCullFactor = 1e-7f;
    const float minArea2 = cell * cell * areaCullFactor;

    std::unordered_set<QuantTriKey, QuantTriKeyHash> unique;
    unique.reserve(src.size() * 2 + 1);

    std::vector<std::array<glm::vec3, 3>> tmp;
    if (out) tmp.reserve(src.size());

    auto quantize = [&](const glm::vec3& p, QuantVtxKey& q, glm::vec3& snapped) {
        q.x = (int32_t)std::lround((p.x - bmin.x) * invCell);
        q.y = (int32_t)std::lround((p.y - bmin.y) * invCell);
        q.z = (int32_t)std::lround((p.z - bmin.z) * invCell);
        snapped.x = bmin.x + (float)q.x * cell;
        snapped.y = bmin.y + (float)q.y * cell;
        snapped.z = bmin.z + (float)q.z * cell;
    };

    QuantVtxKeyLess lessKey;
    for (const auto& tri : src) {
        QuantVtxKey q[3];
        glm::vec3 s[3];
        quantize(tri[0], q[0], s[0]);
        quantize(tri[1], q[1], s[1]);
        quantize(tri[2], q[2], s[2]);

        if (q[0] == q[1] || q[1] == q[2] || q[2] == q[0]) continue;

        glm::vec3 cross = glm::cross(s[1] - s[0], s[2] - s[0]);
        if (glm::dot(cross, cross) <= minArea2) continue;

        std::array<QuantVtxKey, 3> sorted = {q[0], q[1], q[2]};
        std::sort(sorted.begin(), sorted.end(), lessKey);
        QuantTriKey key{sorted[0], sorted[1], sorted[2]};
        if (!unique.insert(key).second) continue;

        if (out) tmp.push_back({s[0], s[1], s[2]});
    }

    if (out) *out = std::move(tmp);
    return out ? out->size() : unique.size();
}

std::vector<std::array<glm::vec3, 3>> SimplifyTrianglesForBudget(
    const std::vector<std::array<glm::vec3, 3>>& src,
    float                                         budgetPct) {
    if (src.empty()) return {};

    budgetPct = std::clamp(budgetPct, 0.1f, 100.f);
    if (budgetPct >= 99.95f) return src;

    const float t = budgetPct / 100.f;
    const float curved = std::pow(t, 1.35f);
    const size_t target = std::max<size_t>(
        1, (size_t)std::lround(curved * (float)src.size()));
    if (target >= src.size()) return src;

    const float areaCullFactor = 1e-6f + (1.f - t) * 0.01f;

    auto absDiff = [](size_t a, size_t b) -> size_t { return a > b ? (a - b) : (b - a); };

    int lowDiv = 1;
    size_t lowCount = BuildQuantizedTriangles(src, lowDiv, areaCullFactor, nullptr);

    int highDiv = 2;
    size_t highCount = BuildQuantizedTriangles(src, highDiv, areaCullFactor, nullptr);
    while (highDiv < 1024 && highCount < target) {
        lowDiv = highDiv;
        lowCount = highCount;
        highDiv *= 2;
        highCount = BuildQuantizedTriangles(src, highDiv, areaCullFactor, nullptr);
    }

    int bestDiv = lowDiv;
    size_t bestCount = lowCount;
    auto consider = [&](int div, size_t count) {
        if (count == 0) return;
        size_t d = absDiff(count, target);
        size_t db = absDiff(bestCount, target);
        if (d < db || (d == db && count > bestCount)) {
            bestDiv = div;
            bestCount = count;
        }
    };

    consider(highDiv, highCount);
    int lo = lowDiv;
    int hi = highDiv;
    size_t loCount = lowCount;
    size_t hiCount = highCount;
    while (lo + 1 < hi) {
        const int mid = lo + (hi - lo) / 2;
        size_t midCount = BuildQuantizedTriangles(src, mid, areaCullFactor, nullptr);
        consider(mid, midCount);
        if (midCount < target) {
            lo = mid;
            loCount = midCount;
        } else {
            hi = mid;
            hiCount = midCount;
        }
    }
    consider(lo, loCount);
    consider(hi, hiCount);

    std::vector<std::array<glm::vec3, 3>> out;
    BuildQuantizedTriangles(src, bestDiv, areaCullFactor, &out);
    if (out.empty() && !src.empty()) out.push_back(src.front());
    return out;
}

} // namespace

PreviewViewport::~PreviewViewport() {
    actor_.Destroy();
    if (simple_fbo_)   glDeleteFramebuffers(1, &simple_fbo_);
    if (simple_color_) glDeleteTextures(1, &simple_color_);
    if (simple_depth_) glDeleteRenderbuffers(1, &simple_depth_);
}

void PreviewViewport::EnsureSimpleFbo_(int w, int h) {
    if (w == simple_w_ && h == simple_h_ && simple_fbo_) return;
    if (simple_fbo_)   glDeleteFramebuffers(1, &simple_fbo_);
    if (simple_color_) glDeleteTextures(1, &simple_color_);
    if (simple_depth_) glDeleteRenderbuffers(1, &simple_depth_);

    glGenTextures(1, &simple_color_);
    glBindTexture(GL_TEXTURE_2D, simple_color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenRenderbuffers(1, &simple_depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, simple_depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    glGenFramebuffers(1, &simple_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, simple_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, simple_color_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, simple_depth_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    simple_w_ = w;
    simple_h_ = h;
}

void PreviewViewport::Init(rco::renderer::Engine*   engine,
                           rco::renderer::Pipeline* pipeline) {
    engine_   = engine;
    pipeline_ = pipeline;
}

void PreviewViewport::Clear() {
    actor_.Destroy();
    current_path_.clear();
    force_full_pipeline_ = false;
    anim_t_ = 0.f;
    anim_actions_.clear();
    sel_action_      = -1;
    sel_action_name_.clear();
    cur_clip_idx_   = -1;
    clip_start_sec_ = 0.f;
    clip_end_sec_   = 0.f;
    collision_mesh_tris_.clear();
    collision_mesh_cache_path_.clear();
    collision_mesh_simplified_tris_.clear();
    collision_mesh_simplified_path_.clear();
    collision_mesh_simplified_budget_ = -1.f;
}

void PreviewViewport::FitCameraToModel() {
    const auto& mdl = actor_.model();
    if (!mdl.IsLoaded()) return;

    const float sc = actor_scale_ > 0.f ? actor_scale_ : 1.f;
    glm::vec3 bmin = mdl.BoundsMin() * sc;
    glm::vec3 bmax = mdl.BoundsMax() * sc;

    // Guard against degenerate / empty AABB (placeholder box or no verts).
    if (bmin.x > bmax.x) {
        bmin = glm::vec3(-0.5f, 0.f, -0.5f) * sc;
        bmax = glm::vec3( 0.5f, 1.f,  0.5f) * sc;
    }

    glm::vec3 center = (bmin + bmax) * 0.5f;
    float     radius = glm::length(bmax - bmin) * 0.5f;
    if (radius < 0.01f) radius = 0.5f;

    // Distance to fit the bounding sphere within the 55° vertical FOV.
    // sin of half-FOV ≈ 0.454 → dist = radius / 0.454 * 1.15 (slight padding).
    const float half_fov_sin = 0.454f;
    float dist = (radius / half_fov_sin) * 1.15f;

    cam_target_   = center;
    cam_dist_     = dist;
    cam_yaw_      = 0.f;
    cam_pitch_    = -15.f;
    cam_near_     = std::max(0.01f,  radius * 0.01f);
    cam_far_      = std::max(200.f,  radius * 10.f);
    cam_dist_min_ = std::max(0.01f,  radius * 0.05f);
    cam_dist_max_ = std::max(50.f,   radius * 20.f);
}

void PreviewViewport::ReloadCurrent() {
    if (current_path_.empty()) return;
    std::string path = current_path_;
    actor_.Destroy();
    // Cache key is whatever was passed to ModelCacheGet — see LoadModel:
    // the resolved (runtime-relative) path.
    rco::renderer::ModelCacheInvalidate(ResolveClientAsset(path));
    current_path_.clear();  // force LoadModel to skip its early-return
    LoadModel(path);
}

bool PreviewViewport::LoadModel(const std::string& path) {
    force_full_pipeline_ = false;
    if (path == current_path_) return true;

    actor_.Destroy();
    current_path_ = path;
    anim_t_ = 0.f;
    // Clear action state so SetAnimActions auto-selects on next call.
    anim_actions_.clear();
    sel_action_      = -1;
    sel_action_name_.clear();
    cur_clip_idx_   = -1;
    clip_start_sec_ = 0.f;
    clip_end_sec_   = 0.f;
    collision_mesh_tris_.clear();
    collision_mesh_cache_path_.clear();
    collision_mesh_simplified_tris_.clear();
    collision_mesh_simplified_path_.clear();
    collision_mesh_simplified_budget_ = -1.f;

    if (path.empty()) return true;

    std::string resolved = ResolveClientAsset(path);
    rco::renderer::MaterialManager* mm = engine_ ? &engine_->materials() : nullptr;
    actor_.Init("", resolved.c_str(), mm);
    if (engine_) engine_->MarkMaterialsDirty();

    // Actor::Init auto-starts "Idle" when the model has animations. If the
    // model ships with clips under another name, fall back to the first one.
    if (actor_.CurrentAnim().empty() && actor_.model().ClipCount() > 0)
        actor_.PlayAnim(actor_.model().ClipName(0), true);

    FitCameraToModel();
    return actor_.IsLoaded();
}

bool PreviewViewport::LoadSpherePrimitive() {
    force_full_pipeline_ = true;
    if (current_path_ == rco::renderer::kSpherePrimitivePath) return true;

    actor_.Destroy();
    current_path_ = rco::renderer::kSpherePrimitivePath;
    anim_t_ = 0.f;
    anim_actions_.clear();
    sel_action_      = -1;
    sel_action_name_.clear();
    cur_clip_idx_   = -1;
    clip_start_sec_ = 0.f;
    clip_end_sec_   = 0.f;
    collision_mesh_tris_.clear();
    collision_mesh_cache_path_.clear();
    collision_mesh_simplified_tris_.clear();
    collision_mesh_simplified_path_.clear();
    collision_mesh_simplified_budget_ = -1.f;

    rco::renderer::MaterialManager* mm = engine_ ? &engine_->materials() : nullptr;
    actor_.Init("", rco::renderer::kSpherePrimitivePath, mm);
    if (engine_) engine_->MarkMaterialsDirty();

    FitCameraToModel();
    return actor_.IsLoaded();
}

void PreviewViewport::SetAttachment(const AttachmentSpec& spec) {
    if (spec.model_path != attachment_path_) {
        attachment_.Destroy();
        attachment_path_ = spec.model_path;
        if (!attachment_path_.empty()) {
            std::string resolved = ResolveClientAsset(attachment_path_);
            rco::renderer::MaterialManager* mm = engine_ ? &engine_->materials() : nullptr;
            attachment_.Init("", resolved.c_str(), mm);
            if (engine_) engine_->MarkMaterialsDirty();
        }
    }
    attachment_spec_ = spec;
    has_attachment_  = !attachment_path_.empty() && attachment_.IsLoaded();
}

void PreviewViewport::ClearAttachment() {
    attachment_.Destroy();
    attachment_path_.clear();
    attachment_spec_ = AttachmentSpec{};
    has_attachment_  = false;
}

void PreviewViewport::ApplyMaterialsFromMedia(const std::vector<MaterialLookup>& mats) {
    if (!actor_.IsLoaded() || !engine_) return;

    auto resolve = [](const std::string& rel) -> std::string {
        if (rel.empty()) return {};
        if (rel.size() >= 2 && (rel[1] == ':' || rel[0] == '/')) return rel;
        return "../client/" + rel;
    };

    std::unordered_map<std::string, rco::renderer::Model::MaterialPaths> by_name;
    for (const auto& m : mats) {
        if (m.name.empty()) continue;
        rco::renderer::Model::MaterialPaths p;
        p.albedo = resolve(m.albedo_rel);
        p.normal = resolve(m.normal_rel);
        p.orm    = resolve(m.orm_rel);
        by_name[m.name] = std::move(p);
    }

    actor_.ApplyMaterialsByName(engine_->materials(), by_name);
    engine_->MarkMaterialsDirty();
}

void PreviewViewport::OverrideMaterial(const std::string& albedo,
                                       const std::string& normal,
                                       const std::string& orm,
                                       float ar, float ag, float ab,
                                       float roughness, float metallic,
                                       bool blackCutout) {
    if (!actor_.IsLoaded()) {
        std::fprintf(stderr,
            "[actor-mat] PreviewViewport::OverrideMaterial called but actor not loaded"
            " (albedo='%s') -> no-op\n", albedo.c_str());
        return;
    }
    std::string r_albedo = ResolveClientAsset(albedo);
    std::string r_normal = ResolveClientAsset(normal);
    std::string r_orm    = ResolveClientAsset(orm);
    std::fprintf(stderr,
        "[actor-mat] PreviewViewport::OverrideMaterial\n"
        "  albedo  raw='%s'  resolved='%s'\n"
        "  normal  raw='%s'  resolved='%s'\n"
        "  orm     raw='%s'  resolved='%s'\n",
        albedo.c_str(), r_albedo.c_str(),
        normal.c_str(), r_normal.c_str(),
        orm.c_str(),    r_orm.c_str());
    actor_.OverrideMaterial(r_albedo, r_normal, r_orm, ar, ag, ab, roughness, metallic,
                            blackCutout,
                            engine_ ? &engine_->materials() : nullptr);
    if (engine_) engine_->MarkMaterialsDirty();
}

void PreviewViewport::OverrideMaterialsByName(
    const std::vector<SubmeshMaterialEntry>& entries) {
    if (!actor_.IsLoaded() || entries.empty()) return;

    std::unordered_map<std::string, rco::renderer::Actor::SubmeshMaterialData> by_name;
    by_name.reserve(entries.size());
    for (const auto& e : entries) {
        rco::renderer::Actor::SubmeshMaterialData d;
        d.albedo_path   = ResolveClientAsset(e.albedo_rel);
        d.normal_path   = ResolveClientAsset(e.normal_rel);
        d.orm_path      = ResolveClientAsset(e.orm_rel);
        d.albedo_factor = {e.albedo_r, e.albedo_g, e.albedo_b};
        d.roughness     = e.roughness;
        d.metallic      = e.metallic;
        d.black_cutout  = e.black_cutout;
        by_name[e.ai_name] = std::move(d);
    }
    actor_.OverrideMaterialsByName(by_name,
                                   engine_ ? &engine_->materials() : nullptr);
    if (engine_) engine_->MarkMaterialsDirty();
}

void PreviewViewport::RenderToEngineFrame_(int w, int h, float dt) {
    if (!engine_ || !pipeline_) return;
    if (w <= 0 || h <= 0) return;

    if (w == last_w_ && h == last_h_) {
        if (stable_frames_ < 2) stable_frames_++;
    } else {
        stable_frames_ = 0;
        last_w_ = w;
        last_h_ = h;
    }
    if (stable_frames_ >= 2) engine_->Resize(w, h);

    float yaw   = glm::radians(cam_yaw_);
    float pitch = glm::radians(cam_pitch_);
    glm::vec3 offset = {
        cam_dist_ * std::cos(pitch) * std::sin(yaw),
        cam_dist_ * std::sin(pitch),
        cam_dist_ * std::cos(pitch) * std::cos(yaw),
    };
    glm::vec3 eye  = cam_target_ + offset;

    // Non-skinned static meshes use a dedicated simple forward shader instead
    // of the deferred bindless pipeline, so plain sampler2D textures work on
    // every driver without GL_ARB_bindless_texture involvement. That fast
    // path only shades flat albedo though (no normal map, no PBR roughness/
    // metallic) — force_full_pipeline_ (set by LoadSpherePrimitive) routes
    // the material-preview sphere through the full deferred pipeline instead
    // so it actually shows the material the way it renders in-game.
    const bool is_static = actor_.IsLoaded() && !force_full_pipeline_
                           && !actor_.model().HasAnimations()
                           && actor_.model().meshes().size() > 0
                           && !actor_.model().meshes()[0].skinned;

    if (is_static) {
        EnsureSimpleFbo_(w, h);

        GLint prev_fbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);

        glBindFramebuffer(GL_FRAMEBUFFER, simple_fbo_);
        glViewport(0, 0, w, h);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glDisable(GL_BLEND);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 proj = glm::perspective(glm::radians(55.0f),
                                          (float)w / (float)h, cam_near_, cam_far_);
        glm::mat4 view = glm::lookAt(eye, cam_target_, glm::vec3(0, 1, 0));
        glm::mat4 vp   = proj * view;

        auto& sh = rco::renderer::Shader::shaders["preview_static"];
        sh->Bind();
        sh->SetMat4("u_viewProj", vp);
        // Actor stays at world origin (no translate/rotate needed for this
        // preview), but scale is NOT baked into the vertex data anywhere
        // (LoadModel loads raw mesh data untouched) — it must be applied
        // here via the model matrix, same as every other draw path in this
        // engine. FIX: was a hardcoded glm::mat4(1.0f) (pure identity),
        // which silently dropped actor_.scale (set every frame by
        // MediaTab::DrawModels via SetActorScale(editModel_.scale), see
        // media.cpp:2240) — static models always drew at scale=1.0
        // regardless of the configured value, while the "Size: WxHxD"
        // ruler (media.cpp:1952) and skinned/animated models (which go
        // through Actor::SubmitAs -> actor.cpp's own
        // glm::scale(model, glm::vec3(scale))) already applied it
        // correctly. Same pattern as the two other glm::scale(...,
        // actor_.scale) call sites in this file (:602, :858) and
        // zone_renderer.cpp:823 — not a new convention.
        sh->SetMat4("u_model", glm::scale(glm::mat4(1.0f), glm::vec3(actor_.scale)));
        sh->SetVec2("u_uvOffset", uv_offset_[0], uv_offset_[1]);
        sh->SetVec2("u_uvScale",  uv_scale_[0],  uv_scale_[1]);
        sh->SetVec3("u_sunDir",   glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)));
        sh->SetVec3("u_sunColor", glm::vec3(1.0f, 0.95f, 0.80f) * sun_intensity_);
        sh->SetFloat("u_ambientStrength", 0.25f);
        sh->SetBool ("u_blackCutout", static_black_cutout_);
        sh->SetFloat("u_blackCutoutThreshold",
                     pipeline_ ? pipeline_->BlackCutoutThreshold() : 0.005f);

        for (const auto& m : actor_.model().meshes()) {
            if (!m.vao || m.idx_count == 0) continue;
            if (m.tex_albedo) {
                glBindTextureUnit(0, m.tex_albedo);
                sh->SetBool("u_hasAlbedo", true);
                sh->SetInt ("u_albedo",    0);
            } else {
                sh->SetBool("u_hasAlbedo",   false);
                sh->SetVec3("u_albedoFactor", m.albedo_factor);
            }
            glBindVertexArray(m.vao);
            glDrawElements(GL_TRIANGLES, m.idx_count, GL_UNSIGNED_INT, nullptr);
        }
        glBindVertexArray(0);

        glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
        return;
    }

    // Skinned / animated models: full deferred pipeline.
    int engW = engine_->width();
    int engH = engine_->height();
    if (engW <= 0 || engH <= 0) return;

    glm::mat4 view = glm::lookAt(eye, cam_target_, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(55.0f),
                                      (float)engW / (float)engH, cam_near_, cam_far_);

    pipeline_->Begin(view, proj, eye, dt);
    pipeline_->SetSun(glm::vec3(-0.4f, -1.0f, -0.3f),
                      glm::vec3(1.0f, 0.95f, 0.80f) * sun_intensity_);

    if (actor_.IsLoaded()) {
        if (playing_) {
            anim_t_ += dt;
            // Loop within [clip_start_sec_, clip_end_sec_] when a range is set.
            const float range = clip_end_sec_ - clip_start_sec_;
            if (range > 0.001f) {
                float local = anim_t_ - clip_start_sec_;
                if (local < 0.f) local = 0.f;
                local   = std::fmod(local, range);
                anim_t_ = clip_start_sec_ + local;
            }
        }
        actor_.SubmitAs(actor_.CurrentAnim(), anim_t_, /*loop*/true, *pipeline_);
    }

    if (has_attachment_) {
        glm::mat4 boneMat(1.0f);
        bool haveBone = !attachment_spec_.bone_name.empty()
            && actor_.GetBoneWorldTransform(attachment_spec_.bone_name, &boneMat);
        if (!attachment_spec_.bone_name.empty() && !haveBone) {
            // Unknown bone name (stale binding) — skip rather than draw at
            // a misleading location.
        } else {
            glm::mat4 actorModel(1.0f);
            actorModel = glm::translate(actorModel, actor_.position + glm::vec3(0.f, actor_.y_offset, 0.f));
            actorModel = glm::rotate(actorModel, glm::radians(actor_.yaw), glm::vec3(0.f, 1.f, 0.f));
            if (actor_.yaw_offset != 0.f)
                actorModel = glm::rotate(actorModel, glm::radians(actor_.yaw_offset), glm::vec3(0.f, 1.f, 0.f));
            actorModel = glm::scale(actorModel, glm::vec3(actor_.scale));

            glm::mat4 world = actorModel * boneMat * attachment_spec_.local_transform;
            attachment_.SubmitWithMatrix(*pipeline_, world);
            attachment_world_ = world;
        }
    }

    rco::renderer::Pipeline::EndConfig cfg{};
    cfg.blit_to_default = false;
    pipeline_->End(cfg);
}

void PreviewViewport::DrawCollisionOverlay_(const ImVec2& image_pos, const ImVec2& image_size) const {
    if (!show_collision_preview_ || collision_shapes_.empty()) return;
    if (image_size.x <= 1.f || image_size.y <= 1.f) return;

    float yaw   = glm::radians(cam_yaw_);
    float pitch = glm::radians(cam_pitch_);
    glm::vec3 offset = {
        cam_dist_ * std::cos(pitch) * std::sin(yaw),
        cam_dist_ * std::sin(pitch),
        cam_dist_ * std::cos(pitch) * std::cos(yaw),
    };
    glm::vec3 eye  = cam_target_ + offset;
    glm::mat4 view = glm::lookAt(eye, cam_target_, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(55.0f),
                                      image_size.x / image_size.y, cam_near_, cam_far_);

    auto project = [&](const glm::vec3& p, ImVec2& out) -> bool {
        glm::vec4 clip = proj * view * glm::vec4(p, 1.f);
        if (clip.w <= 0.0001f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z))
            return false;
        out.x = image_pos.x + (ndc.x * 0.5f + 0.5f) * image_size.x;
        out.y = image_pos.y + (-ndc.y * 0.5f + 0.5f) * image_size.y;
        return true;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return;

    auto addLine3D = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col, float thick) {
        ImVec2 sa, sb;
        if (project(a, sa) && project(b, sb))
            dl->AddLine(sa, sb, col, thick);
    };
    auto addSubdivTriEdges = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                 int subdiv, ImU32 col, float thick) {
        int n = subdiv;
        if (n < 1) n = 1;
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n - i; ++j) {
                float fi0 = (float)i / (float)n;
                float fj0 = (float)j / (float)n;
                float fi1 = (float)(i + 1) / (float)n;
                float fj1 = (float)(j + 1) / (float)n;
                glm::vec3 p0 = a + (b - a) * fi0 + (c - a) * fj0;
                glm::vec3 p1 = a + (b - a) * fi1 + (c - a) * fj0;
                glm::vec3 p2 = a + (b - a) * fi0 + (c - a) * fj1;
                addLine3D(p0, p1, col, thick);
                addLine3D(p1, p2, col, thick);
                addLine3D(p2, p0, col, thick);

                if (i + j < n - 1) {
                    glm::vec3 p3 = a + (b - a) * fi1 + (c - a) * fj1;
                    addLine3D(p1, p3, col, thick);
                    addLine3D(p3, p2, col, thick);
                    addLine3D(p2, p1, col, thick);
                }
            }
        }
    };

    const bool hasMeshShape = std::any_of(
        collision_shapes_.begin(), collision_shapes_.end(),
        [](const CollisionShape& s) { return s.type == 2; });
    if (hasMeshShape) {
        EnsureCollisionMeshCache_();
    }
    float meshBudgetPct = 100.f;
    for (const auto& sh : collision_shapes_) {
        if (sh.type != 2) continue;
        float b = sh.detail_a > 0.f ? sh.detail_a : 100.f;
        if (b < 0.1f) b = 0.1f;
        if (b > 100.f) b = 100.f;
        meshBudgetPct = std::min(meshBudgetPct, b);
    }

    const float globalScale = actor_scale_ > 0.f ? actor_scale_ : 1.f;
    bool meshDrawn = false;
    for (const auto& sh : collision_shapes_) {
        if (sh.type == 0) { // Box
            glm::vec3 c = {sh.offset_x * globalScale, sh.offset_y * globalScale, sh.offset_z * globalScale};
            glm::vec3 h = {std::abs(sh.size_x) * 0.5f * globalScale,
                           std::abs(sh.size_y) * 0.5f * globalScale,
                           std::abs(sh.size_z) * 0.5f * globalScale};
            glm::vec3 v[8] = {
                {c.x-h.x, c.y-h.y, c.z-h.z}, {c.x+h.x, c.y-h.y, c.z-h.z},
                {c.x+h.x, c.y+h.y, c.z-h.z}, {c.x-h.x, c.y+h.y, c.z-h.z},
                {c.x-h.x, c.y-h.y, c.z+h.z}, {c.x+h.x, c.y-h.y, c.z+h.z},
                {c.x+h.x, c.y+h.y, c.z+h.z}, {c.x-h.x, c.y+h.y, c.z+h.z},
            };
            static const int e[12][2] = {
                {0,1},{1,2},{2,3},{3,0},
                {4,5},{5,6},{6,7},{7,4},
                {0,4},{1,5},{2,6},{3,7},
            };
            for (auto& edge : e)
                addLine3D(v[edge[0]], v[edge[1]], IM_COL32(255, 90, 90, 230), 1.6f);
        } else if (sh.type == 1) { // Sphere
            glm::vec3 c = {sh.offset_x * globalScale, sh.offset_y * globalScale, sh.offset_z * globalScale};
            const float r = std::abs(sh.size_x) * globalScale;
            if (r <= 0.f) continue;
            const int segs = 32;
            for (int ring = 0; ring < 3; ++ring) {
                glm::vec3 prev{};
                for (int i = 0; i <= segs; ++i) {
                    float t = (float)i / (float)segs * 6.28318530718f;
                    glm::vec3 cur{};
                    if (ring == 0) cur = {c.x + std::cos(t) * r, c.y + std::sin(t) * r, c.z};
                    if (ring == 1) cur = {c.x, c.y + std::cos(t) * r, c.z + std::sin(t) * r};
                    if (ring == 2) cur = {c.x + std::cos(t) * r, c.y, c.z + std::sin(t) * r};
                    if (i > 0)
                        addLine3D(prev, cur, IM_COL32(255, 160, 40, 230), 1.4f);
                    prev = cur;
                }
            }
        } else if (sh.type == 3) { // Wedge / ramp
            glm::vec3 c = {sh.offset_x * globalScale, sh.offset_y * globalScale, sh.offset_z * globalScale};
            glm::vec3 h = {std::abs(sh.size_x) * 0.5f * globalScale,
                           std::abs(sh.size_y) * 0.5f * globalScale,
                           std::abs(sh.size_z) * 0.5f * globalScale};
            const bool risePositiveX = sh.size_x >= 0.f;
            const float xLow  = risePositiveX ? (c.x - h.x) : (c.x + h.x);
            const float xHigh = risePositiveX ? (c.x + h.x) : (c.x - h.x);
            glm::vec3 v[6] = {
                {xLow,  c.y - h.y, c.z - h.z}, // 0 low-back  bottom
                {xLow,  c.y - h.y, c.z + h.z}, // 1 low-front bottom
                {xHigh, c.y - h.y, c.z - h.z}, // 2 high-back bottom
                {xHigh, c.y + h.y, c.z - h.z}, // 3 high-back top
                {xHigh, c.y + h.y, c.z + h.z}, // 4 high-front top
                {xHigh, c.y - h.y, c.z + h.z}, // 5 high-front bottom
            };
            static const int e[9][2] = {
                {0,1}, {0,2}, {1,5}, {2,5},
                {2,3}, {5,4}, {3,4}, {0,3}, {1,4},
            };
            for (auto& edge : e)
                addLine3D(v[edge[0]], v[edge[1]], IM_COL32(170, 255, 110, 235), 1.7f);
            int subdiv = (int)std::lround(sh.detail_a > 0.f ? sh.detail_a : 1.f);
            if (subdiv < 1) subdiv = 1;
            if (subdiv > 16) subdiv = 16;
            if (subdiv > 1) {
                static const int kTri[8][3] = {
                    {0,1,5}, {0,5,2},
                    {0,1,4}, {0,4,3},
                    {2,5,4}, {2,4,3},
                    {0,2,3}, {1,5,4},
                };
                for (auto& t : kTri)
                    addSubdivTriEdges(v[t[0]], v[t[1]], v[t[2]], subdiv, IM_COL32(170, 255, 110, 110), 1.0f);
            }
        } else { // Mesh (full geometry): draw triangle edges from cached model mesh.
            if (meshDrawn) continue;
            const std::vector<std::array<glm::vec3, 3>>* drawTris = &collision_mesh_tris_;
            float effectiveBudget = meshBudgetPct;
            if (!collision_mesh_tris_.empty()) {
                const float maxPreviewTris = 12000.f;
                const float maxBudgetPct = (maxPreviewTris * 100.f) / (float)collision_mesh_tris_.size();
                if (effectiveBudget > maxBudgetPct)
                    effectiveBudget = maxBudgetPct;
                if (effectiveBudget < 0.1f)
                    effectiveBudget = 0.1f;
            }
            if (!collision_mesh_tris_.empty() &&
                (effectiveBudget < 99.95f || collision_mesh_tris_.size() > 12000)) {
                const bool cacheMiss =
                    collision_mesh_simplified_path_ != collision_mesh_cache_path_ ||
                    std::fabs(collision_mesh_simplified_budget_ - effectiveBudget) > 0.01f ||
                    collision_mesh_simplified_tris_.empty();
                if (cacheMiss) {
                    collision_mesh_simplified_tris_ = SimplifyTrianglesForBudget(collision_mesh_tris_, effectiveBudget);
                    collision_mesh_simplified_path_ = collision_mesh_cache_path_;
                    collision_mesh_simplified_budget_ = effectiveBudget;
                }
                drawTris = &collision_mesh_simplified_tris_;
            }
            if (drawTris->empty()) {
                glm::vec3 c = {sh.offset_x * globalScale, sh.offset_y * globalScale, sh.offset_z * globalScale};
                const float d = 0.08f * globalScale;
                addLine3D({c.x-d, c.y, c.z}, {c.x+d, c.y, c.z}, IM_COL32(255, 220, 70, 220), 1.3f);
                addLine3D({c.x, c.y-d, c.z}, {c.x, c.y+d, c.z}, IM_COL32(255, 220, 70, 220), 1.3f);
                addLine3D({c.x, c.y, c.z-d}, {c.x, c.y, c.z+d}, IM_COL32(255, 220, 70, 220), 1.3f);
                meshDrawn = true;
                continue;
            }
            for (const auto& tri : *drawTris) {
                glm::vec3 a = tri[0] * globalScale;
                glm::vec3 b = tri[1] * globalScale;
                glm::vec3 c = tri[2] * globalScale;
                addLine3D(a, b, IM_COL32(255, 220, 70, 220), 1.0f);
                addLine3D(b, c, IM_COL32(255, 220, 70, 220), 1.0f);
                addLine3D(c, a, IM_COL32(255, 220, 70, 220), 1.0f);
            }
            meshDrawn = true;
        }
    }
}

void PreviewViewport::DrawSocketOverlay_(const ImVec2& image_pos, const ImVec2& image_size) const {
    if (!show_socket_preview_ || socket_preview_.empty()) return;
    if (!actor_.IsLoaded()) return;
    if (image_size.x <= 1.f || image_size.y <= 1.f) return;

    float yaw   = glm::radians(cam_yaw_);
    float pitch = glm::radians(cam_pitch_);
    glm::vec3 offset = {
        cam_dist_ * std::cos(pitch) * std::sin(yaw),
        cam_dist_ * std::sin(pitch),
        cam_dist_ * std::cos(pitch) * std::cos(yaw),
    };
    glm::vec3 eye  = cam_target_ + offset;
    glm::mat4 view = glm::lookAt(eye, cam_target_, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(55.0f),
                                      image_size.x / image_size.y, cam_near_, cam_far_);

    auto project = [&](const glm::vec3& p, ImVec2& out) -> bool {
        glm::vec4 clip = proj * view * glm::vec4(p, 1.f);
        if (clip.w <= 0.0001f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z))
            return false;
        out.x = image_pos.x + (ndc.x * 0.5f + 0.5f) * image_size.x;
        out.y = image_pos.y + (-ndc.y * 0.5f + 0.5f) * image_size.y;
        return true;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return;

    auto addLine3D = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col, float thick) {
        ImVec2 sa, sb;
        if (project(a, sa) && project(b, sb))
            dl->AddLine(sa, sb, col, thick);
    };

    // Actor's object-to-world matrix — mirrors Actor::Submit/SubmitAs so bone
    // world transforms (which are in model space) land in the same place the
    // skinned mesh actually renders.
    glm::mat4 actorModel = glm::mat4(1.0f);
    actorModel = glm::translate(actorModel, actor_.position + glm::vec3(0.f, actor_.y_offset, 0.f));
    actorModel = glm::rotate(actorModel, glm::radians(actor_.yaw), glm::vec3(0.f, 1.f, 0.f));
    if (actor_.yaw_offset != 0.f)
        actorModel = glm::rotate(actorModel, glm::radians(actor_.yaw_offset), glm::vec3(0.f, 1.f, 0.f));
    actorModel = glm::scale(actorModel, glm::vec3(actor_.scale));

    const float axisLen = 0.08f;
    for (const auto& s : socket_preview_) {
        if (s.bone_name.empty()) continue;
        glm::mat4 boneMat;
        if (!actor_.GetBoneWorldTransform(s.bone_name, &boneMat)) continue;

        glm::mat4 offsetMat = glm::translate(glm::mat4(1.0f),
            glm::vec3(s.offset_pos_x, s.offset_pos_y, s.offset_pos_z));
        offsetMat = glm::rotate(offsetMat, glm::radians(s.offset_rot_x), glm::vec3(1, 0, 0));
        offsetMat = glm::rotate(offsetMat, glm::radians(s.offset_rot_y), glm::vec3(0, 1, 0));
        offsetMat = glm::rotate(offsetMat, glm::radians(s.offset_rot_z), glm::vec3(0, 0, 1));

        glm::mat4 world = actorModel * boneMat * offsetMat;
        glm::vec3 origin = glm::vec3(world[3]);
        glm::vec3 xAxis  = origin + glm::vec3(world * glm::vec4(axisLen, 0, 0, 0));
        glm::vec3 yAxis  = origin + glm::vec3(world * glm::vec4(0, axisLen, 0, 0));
        glm::vec3 zAxis  = origin + glm::vec3(world * glm::vec4(0, 0, axisLen, 0));

        addLine3D(origin, xAxis, IM_COL32(255, 70, 70, 255), 2.2f);
        addLine3D(origin, yAxis, IM_COL32(70, 255, 70, 255), 2.2f);
        addLine3D(origin, zAxis, IM_COL32(70, 140, 255, 255), 2.2f);

        ImVec2 sp;
        if (project(origin, sp)) {
            dl->AddCircleFilled(sp, 4.f, IM_COL32(255, 220, 60, 255));
            const std::string label = !s.socket_name.empty() ? s.socket_name : s.bone_name;
            ImVec2 textPos = {sp.x + 6.f, sp.y - 8.f};
            dl->AddText(textPos, IM_COL32(0, 0, 0, 255), label.c_str());
            dl->AddText({textPos.x - 1.f, textPos.y - 1.f}, IM_COL32(255, 255, 0, 255), label.c_str());
        }
    }
}

// Draws the move/rotate axes at the item's current attach point (Items tab
// preview) so dragging the pos/rot sliders has a visible frame of reference
// instead of the item just silently jumping around.
void PreviewViewport::DrawAttachmentGizmo_(const ImVec2& image_pos, const ImVec2& image_size) const {
    if (!has_attachment_) return;
    if (image_size.x <= 1.f || image_size.y <= 1.f) return;

    float yaw   = glm::radians(cam_yaw_);
    float pitch = glm::radians(cam_pitch_);
    glm::vec3 offset = {
        cam_dist_ * std::cos(pitch) * std::sin(yaw),
        cam_dist_ * std::sin(pitch),
        cam_dist_ * std::cos(pitch) * std::cos(yaw),
    };
    glm::vec3 eye  = cam_target_ + offset;
    glm::mat4 view = glm::lookAt(eye, cam_target_, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(55.0f),
                                      image_size.x / image_size.y, cam_near_, cam_far_);

    auto project = [&](const glm::vec3& p, ImVec2& out) -> bool {
        glm::vec4 clip = proj * view * glm::vec4(p, 1.f);
        if (clip.w <= 0.0001f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z))
            return false;
        out.x = image_pos.x + (ndc.x * 0.5f + 0.5f) * image_size.x;
        out.y = image_pos.y + (-ndc.y * 0.5f + 0.5f) * image_size.y;
        return true;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return;

    auto addLine3D = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col, float thick) {
        ImVec2 sa, sb;
        if (project(a, sa) && project(b, sb))
            dl->AddLine(sa, sb, col, thick);
    };

    const glm::mat4& world = attachment_world_;
    glm::vec3 origin = glm::vec3(world[3]);
    // Axis length scales with camera distance so the gizmo stays legible
    // whether the dev is zoomed into a ring or framing the whole actor.
    const float axisLen = std::max(0.05f, cam_dist_ * 0.12f);
    glm::vec3 xTip = origin + glm::normalize(glm::vec3(world[0])) * axisLen;
    glm::vec3 yTip = origin + glm::normalize(glm::vec3(world[1])) * axisLen;
    glm::vec3 zTip = origin + glm::normalize(glm::vec3(world[2])) * axisLen;

    addLine3D(origin, xTip, IM_COL32(255, 60, 60, 255), 3.0f);
    addLine3D(origin, yTip, IM_COL32(60, 255, 60, 255), 3.0f);
    addLine3D(origin, zTip, IM_COL32(70, 140, 255, 255), 3.0f);

    ImVec2 sp;
    if (project(origin, sp))
        dl->AddCircleFilled(sp, 5.f, IM_COL32(255, 255, 255, 230));

    ImVec2 sx, sy, sz;
    if (project(xTip, sx)) dl->AddText(sx, IM_COL32(255, 120, 120, 255), "X");
    if (project(yTip, sy)) dl->AddText(sy, IM_COL32(120, 255, 120, 255), "Y");
    if (project(zTip, sz)) dl->AddText(sz, IM_COL32(140, 180, 255, 255), "Z");
}

void PreviewViewport::DrawScaleOverlay_(const ImVec2& image_pos, const ImVec2& image_size) const {
    if (!actor_.IsLoaded()) return;
    if (image_size.x <= 1.f || image_size.y <= 1.f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return;

    // Recompute view/proj from orbit camera (same as DrawCollisionOverlay_).
    float yaw   = glm::radians(cam_yaw_);
    float pitch = glm::radians(cam_pitch_);
    glm::vec3 offset = {
        cam_dist_ * std::cos(pitch) * std::sin(yaw),
        cam_dist_ * std::sin(pitch),
        cam_dist_ * std::cos(pitch) * std::cos(yaw),
    };
    glm::vec3 eye  = cam_target_ + offset;
    glm::mat4 view = glm::lookAt(eye, cam_target_, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(55.0f),
                                      image_size.x / image_size.y, cam_near_, cam_far_);

    auto project = [&](const glm::vec3& p, ImVec2& out) -> bool {
        glm::vec4 clip = proj * view * glm::vec4(p, 1.f);
        if (clip.w <= 0.0001f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z))
            return false;
        // Clip to ±1.5 NDC so lines approaching the frustum edge don't spike.
        if (ndc.x < -1.5f || ndc.x > 1.5f || ndc.y < -1.5f || ndc.y > 1.5f)
            return false;
        out.x = image_pos.x + (ndc.x * 0.5f + 0.5f) * image_size.x;
        out.y = image_pos.y + (-ndc.y * 0.5f + 0.5f) * image_size.y;
        return true;
    };

    auto addLine3D = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col, float thick) {
        ImVec2 sa, sb;
        if (project(a, sa) && project(b, sb))
            dl->AddLine(sa, sb, col, thick);
    };

    const float sc = actor_scale_ > 0.f ? actor_scale_ : 1.f;
    glm::vec3 bmin = actor_.model().BoundsMin() * sc;
    glm::vec3 bmax = actor_.model().BoundsMax() * sc;

    // ── Ground grid (y = 0 plane) ────────────────────────────────────────────
    // Covers the model footprint + 2-unit margin, clamped to ±20 units.
    float margin = 2.f;
    int gmin = std::max(-20, (int)std::floor(std::min({bmin.x, bmin.z, -margin}) ));
    int gmax = std::min( 20, (int)std::ceil (std::max({bmax.x, bmax.z,  margin}) ));

    for (int i = gmin; i <= gmax; ++i) {
        bool major = (i % 5 == 0);
        ImU32 col  = major ? IM_COL32(170,170,170,110) : IM_COL32(110,110,110,75);
        float thk  = major ? 1.2f : 0.7f;
        // Lines parallel to Z
        addLine3D({(float)i, 0.f, (float)gmin}, {(float)i, 0.f, (float)gmax}, col, thk);
        // Lines parallel to X
        addLine3D({(float)gmin, 0.f, (float)i}, {(float)gmax, 0.f, (float)i}, col, thk);
    }
    // Origin crosshair (bright)
    addLine3D({(float)gmin, 0.f, 0.f}, {(float)gmax, 0.f, 0.f}, IM_COL32(200,200,200,140), 1.4f);
    addLine3D({0.f, 0.f, (float)gmin}, {0.f, 0.f, (float)gmax}, IM_COL32(200,200,200,140), 1.4f);

    // ── Vertical ruler ───────────────────────────────────────────────────────
    // Placed 0.5 units to the right (+X) of the model's bounding box.
    const float rx  = bmax.x + 0.5f;
    const float ry0 = 0.f;               // ruler base at y=0 (ground)
    const float ry1 = bmax.y;            // ruler top at model height
    const ImU32 rCol = IM_COL32(255, 230, 100, 200);

    // Vertical spine
    addLine3D({rx, ry0, 0.f}, {rx, ry1, 0.f}, rCol, 1.5f);

    // Tick marks and labels every 1 unit
    int tickMin = 0;
    int tickMax = (int)std::ceil(ry1);
    for (int t = tickMin; t <= tickMax; ++t) {
        float ty = (float)t;
        bool  major = (t % 5 == 0);
        float hw    = major ? 0.18f : 0.10f;  // half-width of tick
        addLine3D({rx - hw, ty, 0.f}, {rx + hw, ty, 0.f}, rCol, major ? 1.5f : 1.0f);

        // Text label on major ticks (0, 5, 10…) and the last tick
        if (major || t == tickMax) {
            ImVec2 labelPt;
            if (project({rx + 0.22f, ty, 0.f}, labelPt)) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%dm", t);
                dl->AddText(labelPt, IM_COL32(255, 230, 100, 220), buf);
            }
        }
    }
    // Cap line at top
    addLine3D({rx - 0.18f, ry1, 0.f}, {rx + 0.18f, ry1, 0.f}, rCol, 1.5f);
}

void PreviewViewport::EnsureCollisionMeshCache_() const {
    std::string resolved = ResolveClientAsset(current_path_);
    if (resolved == collision_mesh_cache_path_ && !collision_mesh_tris_.empty())
        return;

    collision_mesh_tris_.clear();
    collision_mesh_cache_path_.clear();
    collision_mesh_simplified_tris_.clear();
    collision_mesh_simplified_path_.clear();
    collision_mesh_simplified_budget_ = -1.f;
    if (resolved.empty()) return;

    rco::renderer::ExtractMeshTriangles(resolved, collision_mesh_tris_);
    collision_mesh_cache_path_ = resolved;
}

void PreviewViewport::SetAnimActions(std::vector<AnimActionEntry> actions,
                                     std::function<void(int,int)>  on_set_start,
                                     std::function<void(int,int)>  on_set_end) {
    on_set_start_ = std::move(on_set_start);
    on_set_end_   = std::move(on_set_end);
    bool was_empty = anim_actions_.empty();
    anim_actions_ = std::move(actions);

    // Re-find selected action by name so sel_action_ stays stable across
    // frame-by-frame calls (pointers refresh but index/name stays the same).
    int found = -1;
    for (int i = 0; i < (int)anim_actions_.size(); ++i) {
        if (anim_actions_[i].action == sel_action_name_) { found = i; break; }
    }
    sel_action_ = found;

    // Auto-select and play the first action only when transitioning from empty
    // (i.e. just after LoadModel cleared the list).
    if (was_empty && !anim_actions_.empty()) {
        sel_action_      = 0;
        sel_action_name_ = anim_actions_[0].action;
        PlayActionEntry_(anim_actions_[0]);
    }
}

void PreviewViewport::PlayActionEntry_(const AnimActionEntry& e) {
    const std::string clip_name = e.clip_override.empty() ? e.action : e.clip_override;
    if (!e.source_path.empty()) {
        std::string resolved = ResolveClientAsset(e.source_path);
        actor_.LoadAnim(resolved.c_str(), clip_name.c_str());
    }
    actor_.PlayAnim(clip_name, e.loop);

    // Resolve the actual clip index by mirroring Actor::FindClip (exact match,
    // then case-insensitive prefix match, then clip 0 fallback).  Stored so the
    // scrubber can look up duration by index instead of by name (A1 fix).
    const auto& mdl = actor_.model();
    cur_clip_idx_ = -1;
    for (int i = 0; i < mdl.ClipCount(); ++i)
        if (mdl.ClipName(i) == clip_name) { cur_clip_idx_ = i; break; }
    if (cur_clip_idx_ < 0) {
        std::string low = clip_name;
        for (auto& c : low) c = (char)std::tolower((unsigned char)c);
        for (int i = 0; i < mdl.ClipCount(); ++i) {
            std::string cn = mdl.ClipName(i);
            for (auto& c : cn) c = (char)std::tolower((unsigned char)c);
            if (cn.size() >= low.size() && cn.substr(0, low.size()) == low) {
                cur_clip_idx_ = i; break;
            }
        }
    }
    if (cur_clip_idx_ < 0 && mdl.ClipCount() > 0) cur_clip_idx_ = 0;

    // Compute playback range in seconds from start/end_frame (B2/B3 fix).
    const float fps_raw  = (cur_clip_idx_ >= 0) ? mdl.ClipFps(cur_clip_idx_)      : 30.f;
    const float fps      = fps_raw > 0.f ? fps_raw : 30.f;
    const float total    = (cur_clip_idx_ >= 0) ? mdl.ClipDuration(cur_clip_idx_)  : 0.f;
    clip_start_sec_ = e.start_frame > 0  ? e.start_frame / fps : 0.f;
    clip_end_sec_   = e.end_frame   >= 0 ? e.end_frame   / fps : total;

    anim_t_  = clip_start_sec_;
    playing_ = true;
}

void PreviewViewport::DrawImGui() {
    if (!engine_ || !pipeline_) {
        ImGui::TextDisabled("Preview unavailable (engine/pipeline not initialized).");
        return;
    }

    ImVec2 region = ImGui::GetContentRegionAvail();
    if (region.x < 64.f) region.x = 64.f;
    if (region.y < 64.f) region.y = 64.f;

    const float controls_h = 56.f;  // two rows: Play/dropdown/Reset/Light + scrubber
    ImVec2 view_size = { region.x, std::max(64.f, region.y - controls_h - 4.f) };

    int w = (int)view_size.x;
    int h = (int)view_size.y;

    ImVec2 cursor_before = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pv_drag", view_size, ImGuiButtonFlags_MouseButtonLeft);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();
    if (active) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        cam_yaw_   += d.x * 0.5f;
        cam_pitch_ -= d.y * 0.3f;
        cam_pitch_ = std::clamp(cam_pitch_, -80.f, 80.f);
    }
    if (hovered) {
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.f) {
            cam_dist_ *= std::pow(0.9f, scroll);
            cam_dist_ = std::clamp(cam_dist_, cam_dist_min_, cam_dist_max_);
        }
    }

    GLint prev_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    RenderToEngineFrame_(w, h, ImGui::GetIO().DeltaTime);

    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);

    ImGui::SetCursorScreenPos(cursor_before);
    // Static meshes render into simple_fbo_ (no bindless); skinned models use
    // the engine's deferred pipeline output.
    const bool used_simple = simple_fbo_ != 0
        && actor_.IsLoaded()
        && !actor_.model().HasAnimations()
        && !actor_.model().meshes().empty()
        && !actor_.model().meshes()[0].skinned;
    ImTextureID img_id = used_simple
        ? (ImTextureID)(intptr_t)simple_color_
        : (ImTextureID)(intptr_t)engine_->finalImage();
    // Both FBOs have Y=0 at the bottom (OpenGL convention); flip for ImGui.
    ImVec2 uv0 = ImVec2(0.f, 1.f);
    ImVec2 uv1 = ImVec2(1.f, 0.f);
    ImGui::Image(img_id, view_size, uv0, uv1);
    DrawCollisionOverlay_(cursor_before, view_size);
    DrawScaleOverlay_(cursor_before, view_size);
    DrawSocketOverlay_(cursor_before, view_size);
    DrawAttachmentGizmo_(cursor_before, view_size);

    const auto& mdl = actor_.model();
    float scrub_dur = 0.f;
    float scrub_fps = 30.f;

    const bool has_actions   = !anim_actions_.empty();
    const bool has_raw_clips = mdl.HasAnimations() && mdl.ClipCount() > 0;

    if (has_actions || has_raw_clips) {
        ImGui::Checkbox("Play", &playing_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);

        if (has_actions) {
            // Dropdown lists configured ACTIONS (Idle, Walk, Attack…).
            const char* sel_label = (sel_action_ >= 0 && sel_action_ < (int)anim_actions_.size())
                ? anim_actions_[sel_action_].action.c_str() : "(none)";
            if (ImGui::BeginCombo("##clip", sel_label)) {
                for (int i = 0; i < (int)anim_actions_.size(); ++i) {
                    bool is_sel = (i == sel_action_);
                    if (ImGui::Selectable(anim_actions_[i].action.c_str(), is_sel)) {
                        sel_action_      = i;
                        sel_action_name_ = anim_actions_[i].action;
                        PlayActionEntry_(anim_actions_[i]);
                    }
                    if (is_sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else {
            // Fallback when no actions are configured: show raw clip names.
            const std::string& cur = actor_.CurrentAnim();
            if (ImGui::BeginCombo("##clip", cur.empty() ? "(none)" : cur.c_str())) {
                for (int i = 0; i < mdl.ClipCount(); ++i) {
                    bool is_sel = (mdl.ClipName(i) == cur);
                    if (ImGui::Selectable(mdl.ClipName(i).c_str(), is_sel)) {
                        actor_.PlayAnim(mdl.ClipName(i), true);
                        anim_t_ = 0.f;
                    }
                    if (is_sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        // A1: resolve scrub duration by clip index (not name) so Mixamo models
        // whose native clip name differs from the action name still show a scrubber.
        if (cur_clip_idx_ >= 0 && cur_clip_idx_ < mdl.ClipCount()) {
            scrub_dur = mdl.ClipDuration(cur_clip_idx_);
            scrub_fps = mdl.ClipFps(cur_clip_idx_);
        }
        ImGui::SameLine();
    }

    // B4: refresh playback range every frame from the selected action's current
    // start/end_frame values.  Entries are rebuilt each frame from anim_map so
    // table edits propagate here without requiring a dropdown re-selection.
    if (has_actions && sel_action_ >= 0 && sel_action_ < (int)anim_actions_.size()
            && scrub_dur > 0.f) {
        const AnimActionEntry& ae = anim_actions_[sel_action_];
        const float fps_safe = scrub_fps > 0.f ? scrub_fps : 30.f;
        clip_start_sec_ = ae.start_frame > 0  ? ae.start_frame / fps_safe : 0.f;
        clip_end_sec_   = ae.end_frame   >= 0 ? ae.end_frame   / fps_safe : scrub_dur;
        // If anim_t_ fell outside the (possibly tightened) range, snap to start.
        if (clip_start_sec_ < clip_end_sec_
                && (anim_t_ < clip_start_sec_ || anim_t_ >= clip_end_sec_)) {
            anim_t_ = clip_start_sec_;
        }
    }

    if (ImGui::Button("Reset cam")) FitCameraToModel();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.f);
    ImGui::SliderFloat("Light", &sun_intensity_, 0.0f, 4.0f, "%.2f");

    // Scrubber + Set Start / Set End frame markers.
    if (scrub_dur > 0.f) {
        ImGui::SetNextItemWidth(300.f);
        if (ImGui::SliderFloat("##scrub", &anim_t_, 0.f, scrub_dur, "%.3f s"))
            playing_ = false;
        ImGui::SameLine();

        const float fps_step = scrub_fps > 0.f ? scrub_fps : 30.f;
        if (ImGui::ArrowButton("##frame_prev", ImGuiDir_Left)) {
            playing_ = false;
            anim_t_ -= 1.f / fps_step;
            if (anim_t_ < 0.f) anim_t_ = 0.f;
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##frame_next", ImGuiDir_Right)) {
            playing_ = false;
            anim_t_ += 1.f / fps_step;
            if (anim_t_ > scrub_dur) anim_t_ = scrub_dur;
        }
        ImGui::SameLine();
        int cur_frame = static_cast<int>(anim_t_ * scrub_fps);
        ImGui::Text("Frame %d  (%.1f fps)", cur_frame, scrub_fps);

        // Resolve action_index at click time via callbacks — robust to any
        // realloc of editActorDef_.anim_map between frames. Dev clicks Save
        // in the table row to persist — no auto-save.
        if (has_actions && sel_action_ >= 0 && sel_action_ < (int)anim_actions_.size()) {
            int idx = anim_actions_[sel_action_].action_index;
            ImGui::SameLine();
            if (ImGui::SmallButton("Set Start") && on_set_start_)
                on_set_start_(idx, cur_frame);
            ImGui::SameLine();
            if (ImGui::SmallButton("Set End") && on_set_end_)
                on_set_end_(idx, cur_frame);
        }
    }

    // UV transform diagnostic — visible only for static (non-skinned) meshes
    // since they use the simple preview shader.
    const bool show_uv_panel = actor_.IsLoaded()
        && !actor_.model().HasAnimations()
        && !actor_.model().meshes().empty()
        && !actor_.model().meshes()[0].skinned;
    if (show_uv_panel) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset UV")) {
            uv_offset_[0] = 0.f; uv_offset_[1] = 0.f;
            uv_scale_[0]  = 1.f; uv_scale_[1]  = 1.f;
        }
        ImGui::SetNextItemWidth(180.f);
        ImGui::DragFloat2("UV offset", uv_offset_, 0.001f, -1.f, 1.f, "%.3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.f);
        ImGui::DragFloat2("UV scale",  uv_scale_,  0.005f,  0.1f, 8.f, "%.2f");
    }
}

} // namespace gue
