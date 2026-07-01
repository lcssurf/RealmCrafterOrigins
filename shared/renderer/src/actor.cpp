#include "rco/renderer/actor.h"
#include "rco/renderer/model_cache.h"
#include "rco/renderer/material.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <cstdio>
#include <string>

#include "rco/renderer/pipeline.h"

namespace rco::renderer {

std::atomic<uint64_t> Actor::s_next_actor_id_{1};


// ---------------------------------------------------------------------------
// Init / LoadAnim / Destroy
// ---------------------------------------------------------------------------

void Actor::Init(const char* /*shader_dir*/, const char* model_path,
                 MaterialManager* mm) {
    instance_id_ = s_next_actor_id_++;
    model_ = ModelCacheGet(model_path, mm);
    mesh_material_overrides_.clear();

    if (model_->HasAnimations() && model_->ClipCount() > 0)
        PlayAnim(model_->ClipName(0).c_str(), true);
}

void Actor::LoadAnim(const char* path, const char* name) {
    if (model_) model_->AppendAnimationsFrom(path, name);
}

void Actor::Destroy() {
    for (GLuint& id : bone_ssbos_) {
        if (id) { glDeleteBuffers(1, &id); id = 0; }
    }
    bone_ssbos_.clear();
    mesh_material_overrides_.clear();
    model_.reset();
}

// ---------------------------------------------------------------------------
// OverrideMaterial — per-actor SSBO entry allocation
// ---------------------------------------------------------------------------

void Actor::OverrideMaterial(const std::string& albedo_path,
                             const std::string& normal_path,
                             const std::string& orm_path,
                             float albedo_r, float albedo_g, float albedo_b,
                             float roughness, float metallic,
                             bool blackCutout,
                             MaterialManager* mm) {
    if (!model_) return;
    // Update shared SubMesh fields (tex_albedo, factors) + emit diag logs.
    model_->OverrideMaterial(albedo_path, normal_path, orm_path,
                             albedo_r, albedo_g, albedo_b,
                             roughness, metallic);
    if (!mm) return;

    // Allocate per-actor SSBO entries. Key includes this pointer so two actors
    // pointing to the same shared Model get independent slots.
    const auto& meshes = model_->meshes();
    mesh_material_overrides_.resize(meshes.size(), -1);
    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto& m = meshes[i];
        const std::string key = "actor_override:" + std::to_string(instance_id_) + "#" + std::to_string(i);
        mesh_material_overrides_[i] = mm->RegisterFromHandles(
            key,
            m.tex_albedo, m.tex_normal, m.tex_orm,
            m.tex_opacity, m.tex_ao,
            m.orm_packed,
            m.albedo_factor,
            m.roughness_factor,
            m.metallic_factor,
            1.0f,
            blackCutout);
    }
}

// ---------------------------------------------------------------------------
// OverrideMaterialsByName — per-submesh material override by aiMaterial name
// ---------------------------------------------------------------------------

void Actor::OverrideMaterialsByName(
    const std::unordered_map<std::string, SubmeshMaterialData>& by_name,
    MaterialManager* mm) {
    if (!model_ || by_name.empty()) return;

    // Build Model::MaterialPaths for ApplyMaterialsByName (paths only).
    std::unordered_map<std::string, Model::MaterialPaths> paths;
    paths.reserve(by_name.size());
    for (const auto& [name, data] : by_name)
        paths[name] = {data.albedo_path, data.normal_path, data.orm_path};

    // Load textures into SubMesh.tex_* and update the shared model's material_idx
    // for the named submeshes. Side-effect on the shared model is acceptable
    // (same as the existing ApplyMaterialsByName path via zone_renderer/client).
    if (mm) model_->ApplyMaterialsByName(*mm, paths);

    // Allocate per-actor SSBO entries for overridden submeshes, leaving
    // mesh_material_overrides_[i] == -1 for submeshes with no override so
    // they continue to use the model's shared material_idx.
    const auto& meshes = model_->meshes();
    mesh_material_overrides_.assign(meshes.size(), -1);
    if (!mm) return;
    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto& m = meshes[i];
        auto it = by_name.find(m.material_name);
        if (it == by_name.end()) continue;

        const SubmeshMaterialData& d = it->second;
        const std::string key = "actor_override:" + std::to_string(instance_id_)
                                + "#" + std::to_string(i);
        // Use tex handles loaded by ApplyMaterialsByName above, but factors
        // from the user-supplied override (not the model file's defaults).
        mesh_material_overrides_[i] = mm->RegisterFromHandles(
            key,
            m.tex_albedo, m.tex_normal, m.tex_orm,
            m.tex_opacity, m.tex_ao,
            m.orm_packed,
            d.albedo_factor,
            d.roughness,
            d.metallic,
            1.0f,
            d.black_cutout);
    }
}

// ---------------------------------------------------------------------------
// Clip lookup
// ---------------------------------------------------------------------------

int Actor::FindClip(const std::string& name) const {
    if (!model_) return -1;
    for (int i = 0; i < model_->ClipCount(); ++i)
        if (model_->ClipName(i) == name) return i;
    for (int i = 0; i < model_->ClipCount(); ++i) {
        const std::string& cn = model_->ClipName(i);
        if (cn.size() >= name.size()) {
            bool match = true;
            for (size_t j = 0; j < name.size(); ++j)
                if (std::tolower((unsigned char)cn[j]) !=
                    std::tolower((unsigned char)name[j]))
                { match = false; break; }
            if (match) return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// PlayAnim
// ---------------------------------------------------------------------------

void Actor::PlayAnim(const std::string& name, bool loop,
                     const std::string& return_to, bool restart) {
    int idx = FindClip(name);
    if (idx < 0) {
        if (model_ && model_->ClipCount() > 0) idx = 0;
        else return;
    }

    if (idx == clip_idx_ && !restart) {
        loop_      = loop;
        return_to_ = return_to;
        cur_name_  = name;
        return;
    }

    // Start a crossfade when switching to a different clip
    if (idx != clip_idx_ && clip_idx_ >= 0 && blend_dur > 0.f) {
        from_clip_idx_ = clip_idx_;
        from_name_     = cur_name_;
        from_t_        = anim_t_;
        blend_t_       = 0.f;
    } else {
        blend_t_ = blend_dur;  // no previous clip or same-clip restart — skip blend
    }

    clip_idx_  = idx;
    anim_t_    = 0.f;
    loop_      = loop;
    return_to_ = return_to;
    cur_name_  = name;
}

// ---------------------------------------------------------------------------
// Update — advance time, handle one-shot end
// ---------------------------------------------------------------------------

void Actor::Update(float dt) {
    if (!model_ || !model_->HasAnimations() || clip_idx_ < 0) return;

    anim_t_ += dt;

    // Keep the from-clip advancing and track blend progress
    if (blend_t_ < blend_dur) {
        from_t_  += dt;
        blend_t_ += dt;
    }

    if (!loop_) {
        float dur = model_->ClipDuration(clip_idx_);
        if (dur > 0.f && anim_t_ >= dur) {
            anim_t_ = dur;
            if (!return_to_.empty()) {
                PlayAnim(return_to_, true);
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Bone SSBO helpers
// ---------------------------------------------------------------------------

void Actor::EnsureBoneSSBOs_(size_t count) {
    while (bone_ssbos_.size() < count) {
        GLuint id = 0;
        glCreateBuffers(1, &id);
        glNamedBufferData(id, sizeof(glm::mat4) * kMaxBones, nullptr, GL_DYNAMIC_DRAW);
        bone_ssbos_.push_back(id);
    }
}

void Actor::UploadBonesToSSBO_(size_t mesh_idx, const glm::mat4* bones, int count) {
    if (mesh_idx >= bone_ssbos_.size()) EnsureBoneSSBOs_(mesh_idx + 1);
    int n = std::min(count, kMaxBones);
    glNamedBufferSubData(bone_ssbos_[mesh_idx], 0, sizeof(glm::mat4) * n, bones);
}

// ---------------------------------------------------------------------------
// FillBlendedBones_ — SLERP blend per-bone between two clips into `out`
// ---------------------------------------------------------------------------

void Actor::FillBlendedBones_(int cidx_from, float ft, int cidx_to, float tt,
                               float alpha, int mesh_idx,
                               glm::mat4* out, int max) const {
    model_->ComputeBlendedBones(cidx_from, ft, cidx_to, tt, alpha, mesh_idx, out, max);
}

// ---------------------------------------------------------------------------
// Submit / SubmitAs — build a DynamicDrawRequest per submesh
// ---------------------------------------------------------------------------

void Actor::Submit(Pipeline& pipeline) {
    if (!model_ || !model_->IsLoaded()) return;

    const auto& meshes = model_->meshes();
    const bool diag    = model_->TakeSubmitDiagLog();

    if (diag)
        std::fprintf(stderr,
            "[actor-mat] Actor::Submit: %zu meshes total\n", meshes.size());

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, position + glm::vec3(0.f, y_offset, 0.f));
    model = glm::rotate(model, glm::radians(yaw), glm::vec3(0.f, 1.f, 0.f));
    if (yaw_offset != 0.f)
        model = glm::rotate(model, glm::radians(yaw_offset), glm::vec3(0.f, 1.f, 0.f));
    model = glm::scale(model, glm::vec3(scale));

    bool  blending     = blend_t_ < blend_dur && from_clip_idx_ >= 0;
    float blend_alpha  = blending ? glm::smoothstep(0.f, blend_dur, blend_t_) : 1.f;

    glm::mat4 bones[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        const int mat_idx = (mi < mesh_material_overrides_.size() && mesh_material_overrides_[mi] >= 0)
                            ? mesh_material_overrides_[mi] : m.material_idx;

        if (diag)
            std::fprintf(stderr,
                "[actor-mat]   submit mesh[%zu] '%s' skinned=%d"
                "  mat_idx=%d (override=%d model_idx=%d)"
                "  tex_albedo=%u (override wrote this, DRAW IGNORES for deferred)\n",
                mi, m.material_name.c_str(), (int)m.skinned,
                mat_idx,
                (mi < mesh_material_overrides_.size() ? mesh_material_overrides_[mi] : -1),
                m.material_idx, m.tex_albedo);

        if (m.skinned) {
            if (blending)
                FillBlendedBones_(from_clip_idx_, from_t_, clip_idx_, anim_t_,
                                  blend_alpha, (int)mi, bones, kMaxBones);
            else
                model_->ComputeBones(clip_idx_, anim_t_, (int)mi, bones, kMaxBones);

            SkinnedInstancedEntry e{};
            e.vao          = m.vao;
            e.ebo          = m.ebo;
            e.index_count  = m.idx_count;
            e.material_idx = mat_idx;
            e.model        = model;
            e.readability_mask = ReadabilityMask_();
            for (int b = 0; b < kMaxBones; ++b) e.bones[b] = bones[b];
            pipeline.SubmitSkinnedInstanced(e);
        } else {
            DynamicDrawRequest req{};
            req.vao          = m.vao;
            req.vbo          = m.vbo;
            req.ebo          = m.ebo;
            req.index_count  = m.idx_count;
            req.material_idx = mat_idx;
            req.tex_albedo   = m.tex_albedo;
            req.model        = model;
            req.readability_mask = ReadabilityMask_();
            pipeline.SubmitDynamic(req);
        }
    }
}

void Actor::SubmitWithMatrix(Pipeline& pipeline, const glm::mat4& model_matrix) {
    if (!model_ || !model_->IsLoaded()) return;

    const auto& meshes = model_->meshes();

    bool  blending    = blend_t_ < blend_dur && from_clip_idx_ >= 0;
    float blend_alpha = blending ? glm::smoothstep(0.f, blend_dur, blend_t_) : 1.f;

    glm::mat4 bones[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        const int mat_idx = (mi < mesh_material_overrides_.size() && mesh_material_overrides_[mi] >= 0)
                            ? mesh_material_overrides_[mi] : m.material_idx;
        if (m.skinned) {
            if (blending)
                FillBlendedBones_(from_clip_idx_, from_t_, clip_idx_, anim_t_,
                                  blend_alpha, (int)mi, bones, kMaxBones);
            else
                model_->ComputeBones(clip_idx_, anim_t_, (int)mi, bones, kMaxBones);

            SkinnedInstancedEntry e{};
            e.vao          = m.vao;
            e.ebo          = m.ebo;
            e.index_count  = m.idx_count;
            e.material_idx = mat_idx;
            e.model        = model_matrix;
            e.readability_mask = ReadabilityMask_();
            for (int b = 0; b < kMaxBones; ++b) e.bones[b] = bones[b];
            pipeline.SubmitSkinnedInstanced(e);
        } else {
            DynamicDrawRequest req{};
            req.vao          = m.vao;
            req.vbo          = m.vbo;
            req.ebo          = m.ebo;
            req.index_count  = m.idx_count;
            req.material_idx = mat_idx;
            req.tex_albedo   = m.tex_albedo;
            req.model        = model_matrix;
            req.readability_mask = ReadabilityMask_();
            pipeline.SubmitDynamic(req);
        }
    }
}

void Actor::SubmitAs(const std::string& anim_name, float anim_t, bool loop,
                     Pipeline& pipeline) {
    if (!model_ || !model_->IsLoaded()) return;

    const auto& meshes = model_->meshes();

    // Resolve target clip once
    int cidx = -1;
    float t = 0.f;
    if (model_->HasAnimations()) {
        cidx = FindClip(anim_name);
        if (cidx < 0 && model_->ClipCount() > 0) cidx = 0;
        if (cidx >= 0) {
            float dur = model_->ClipDuration(cidx);
            t = anim_t;
            if (loop) { if (dur > 0.f) t = std::fmod(anim_t, dur); }
            else      { if (dur > 0.f && t > dur) t = dur; }
        }
    }

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, position + glm::vec3(0.f, y_offset, 0.f));
    model = glm::rotate(model, glm::radians(yaw), glm::vec3(0.f, 1.f, 0.f));
    if (yaw_offset != 0.f)
        model = glm::rotate(model, glm::radians(yaw_offset), glm::vec3(0.f, 1.f, 0.f));
    model = glm::scale(model, glm::vec3(scale));

    glm::mat4 bones[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        const int mat_idx = (mi < mesh_material_overrides_.size() && mesh_material_overrides_[mi] >= 0)
                            ? mesh_material_overrides_[mi] : m.material_idx;
        if (m.skinned) {
            model_->ComputeBones(cidx, t, (int)mi, bones, kMaxBones);
            SkinnedInstancedEntry e{};
            e.vao          = m.vao;
            e.ebo          = m.ebo;
            e.index_count  = m.idx_count;
            e.material_idx = mat_idx;
            e.model        = model;
            e.readability_mask = ReadabilityMask_();
            for (int b = 0; b < kMaxBones; ++b) e.bones[b] = bones[b];
            pipeline.SubmitSkinnedInstanced(e);
        } else {
            DynamicDrawRequest req{};
            req.vao          = m.vao;
            req.vbo          = m.vbo;
            req.ebo          = m.ebo;
            req.index_count  = m.idx_count;
            req.material_idx = mat_idx;
            req.tex_albedo   = m.tex_albedo;
            req.model        = model;
            req.readability_mask = ReadabilityMask_();
            pipeline.SubmitDynamic(req);
        }
    }
}

// ---------------------------------------------------------------------------
// SubmitBlended — SLERP blend between two clips per bone
// ---------------------------------------------------------------------------

void Actor::SubmitBlended(Pipeline& pipeline,
                          const std::string& from_anim, float from_t,
                          const std::string& to_anim,   float to_t,
                          float blend_alpha) {
    if (!model_ || !model_->IsLoaded()) return;
    if (blend_alpha <= 0.001f) { SubmitAs(from_anim, from_t, true, pipeline); return; }
    if (blend_alpha >= 0.999f) { SubmitAs(to_anim,   to_t,   true, pipeline); return; }

    const auto& meshes = model_->meshes();
    EnsureBoneSSBOs_(meshes.size());

    int cidx_from = FindClip(from_anim);
    int cidx_to   = FindClip(to_anim);
    if (cidx_from < 0) cidx_from = (model_->ClipCount() > 0) ? 0 : -1;
    if (cidx_to   < 0) cidx_to   = (model_->ClipCount() > 0) ? 0 : -1;
    // If either clip is invalid, fall back to to_anim
    if (cidx_from < 0 || cidx_to < 0) { SubmitAs(to_anim, to_t, true, pipeline); return; }

    // Clamp time into clip duration (loop)
    auto clamped = [&](int idx, float t) {
        float dur = model_->ClipDuration(idx);
        return (dur > 0.f) ? std::fmod(t, dur) : 0.f;
    };
    float fa = clamped(cidx_from, from_t);
    float ta = clamped(cidx_to,   to_t);

    glm::mat4 model_matrix = glm::mat4(1.f);
    model_matrix = glm::translate(model_matrix, position + glm::vec3(0.f, y_offset, 0.f));
    model_matrix = glm::rotate(model_matrix, glm::radians(yaw), glm::vec3(0.f, 1.f, 0.f));
    if (yaw_offset != 0.f)
        model_matrix = glm::rotate(model_matrix, glm::radians(yaw_offset), glm::vec3(0.f, 1.f, 0.f));
    model_matrix = glm::scale(model_matrix, glm::vec3(scale));

    glm::mat4 bones_b[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        const int mat_idx = (mi < mesh_material_overrides_.size() && mesh_material_overrides_[mi] >= 0)
                            ? mesh_material_overrides_[mi] : m.material_idx;
        if (m.skinned) {
            model_->ComputeBlendedBones(cidx_from, fa, cidx_to, ta,
                                        blend_alpha, (int)mi, bones_b, kMaxBones);
            UploadBonesToSSBO_(mi, bones_b, kMaxBones);
        }
        DynamicDrawRequest req{};
        req.vao         = m.vao;
        req.vbo         = m.vbo;
        req.ebo         = m.ebo;
        req.index_count = m.idx_count;
        req.material_idx= mat_idx;
        req.model       = model_matrix;
        req.bone_ssbo   = m.skinned ? bone_ssbos_[mi] : 0;
        req.bone_count  = kMaxBones;
        req.readability_mask = ReadabilityMask_();
        if (m.skinned) pipeline.SubmitSkinned(req);
        else           pipeline.SubmitDynamic(req);
    }
}

} // namespace rco::renderer
