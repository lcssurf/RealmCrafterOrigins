#include "rco/renderer/actor.h"
#include "rco/renderer/model_cache.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdio>
#include <cmath>

#include "rco/renderer/pipeline.h"

namespace rco::renderer {

// ---------------------------------------------------------------------------
// TRS decompose / recompose helpers for per-bone SLERP blending
// ---------------------------------------------------------------------------

static void DecomposeTRS(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    t = glm::vec3(m[3]);
    s.x = glm::length(glm::vec3(m[0]));
    s.y = glm::length(glm::vec3(m[1]));
    s.z = glm::length(glm::vec3(m[2]));
    glm::mat3 rot;
    rot[0] = glm::vec3(m[0]) / (s.x > 1e-4f ? s.x : 1.f);
    rot[1] = glm::vec3(m[1]) / (s.y > 1e-4f ? s.y : 1.f);
    rot[2] = glm::vec3(m[2]) / (s.z > 1e-4f ? s.z : 1.f);
    r = glm::quat_cast(rot);
}

static glm::mat4 ComposeTRS(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    glm::mat4 m = glm::mat4_cast(r);
    m[0] *= s.x; m[1] *= s.y; m[2] *= s.z;
    m[3] = glm::vec4(t, 1.f);
    return m;
}

// ---------------------------------------------------------------------------
// Init / LoadAnim / Destroy
// ---------------------------------------------------------------------------

void Actor::Init(const char* /*shader_dir*/, const char* model_path,
                 MaterialManager* mm) {
    model_ = ModelCacheGet(model_path, mm);

    if (model_->HasAnimations())
        PlayAnim("Idle", true);
}

void Actor::LoadAnim(const char* path, const char* name) {
    if (model_) model_->AppendAnimationsFrom(path, name);
}

void Actor::Destroy() {
    for (GLuint& id : bone_ssbos_) {
        if (id) { glDeleteBuffers(1, &id); id = 0; }
    }
    bone_ssbos_.clear();
    model_.reset();
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

    // Bone matrices are now computed per-submesh in Submit() because each
    // submesh in a multi-part model has its own bind-pose offsets.
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
// Submit / SubmitAs — build a DynamicDrawRequest per submesh
// ---------------------------------------------------------------------------

void Actor::Submit(Pipeline& pipeline) {
    if (!model_ || !model_->IsLoaded()) return;

    const auto& meshes = model_->meshes();

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, position + glm::vec3(0.f, y_offset, 0.f));
    model = glm::rotate(model, glm::radians(yaw), glm::vec3(0.f, 1.f, 0.f));
    if (yaw_offset != 0.f)
        model = glm::rotate(model, glm::radians(yaw_offset), glm::vec3(0.f, 1.f, 0.f));
    model = glm::scale(model, glm::vec3(scale));

    glm::mat4 bones[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        if (m.skinned) {
            model_->ComputeBones(clip_idx_, anim_t_, (int)mi, bones, kMaxBones);
            SkinnedInstancedEntry e{};
            e.vao          = m.vao;
            e.ebo          = m.ebo;
            e.index_count  = m.idx_count;
            e.material_idx = m.material_idx;
            e.model        = model;
            for (int b = 0; b < kMaxBones; ++b) e.bones[b] = bones[b];
            pipeline.SubmitSkinnedInstanced(e);
        } else {
            DynamicDrawRequest req{};
            req.vao          = m.vao;
            req.vbo          = m.vbo;
            req.ebo          = m.ebo;
            req.index_count  = m.idx_count;
            req.material_idx = m.material_idx;
            req.model        = model;
            pipeline.SubmitDynamic(req);
        }
    }
}

void Actor::SubmitWithMatrix(Pipeline& pipeline, const glm::mat4& model_matrix) {
    if (!model_ || !model_->IsLoaded()) return;

    const auto& meshes = model_->meshes();

    glm::mat4 bones[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        if (m.skinned) {
            model_->ComputeBones(clip_idx_, anim_t_, (int)mi, bones, kMaxBones);
            SkinnedInstancedEntry e{};
            e.vao          = m.vao;
            e.ebo          = m.ebo;
            e.index_count  = m.idx_count;
            e.material_idx = m.material_idx;
            e.model        = model_matrix;
            for (int b = 0; b < kMaxBones; ++b) e.bones[b] = bones[b];
            pipeline.SubmitSkinnedInstanced(e);
        } else {
            DynamicDrawRequest req{};
            req.vao          = m.vao;
            req.vbo          = m.vbo;
            req.ebo          = m.ebo;
            req.index_count  = m.idx_count;
            req.material_idx = m.material_idx;
            req.model        = model_matrix;
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
        if (m.skinned) {
            model_->ComputeBones(cidx, t, (int)mi, bones, kMaxBones);
            SkinnedInstancedEntry e{};
            e.vao          = m.vao;
            e.ebo          = m.ebo;
            e.index_count  = m.idx_count;
            e.material_idx = m.material_idx;
            e.model        = model;
            for (int b = 0; b < kMaxBones; ++b) e.bones[b] = bones[b];
            pipeline.SubmitSkinnedInstanced(e);
        } else {
            DynamicDrawRequest req{};
            req.vao          = m.vao;
            req.vbo          = m.vbo;
            req.ebo          = m.ebo;
            req.index_count  = m.idx_count;
            req.material_idx = m.material_idx;
            req.model        = model;
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

    glm::mat4 bones_f[kMaxBones], bones_t[kMaxBones], bones_b[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        if (m.skinned) {
            model_->ComputeBones(cidx_from, fa, (int)mi, bones_f, kMaxBones);
            model_->ComputeBones(cidx_to,   ta, (int)mi, bones_t, kMaxBones);
            for (int bi = 0; bi < kMaxBones; ++bi) {
                glm::vec3 tf, tt, sf, st;
                glm::quat rf, rt;
                DecomposeTRS(bones_f[bi], tf, rf, sf);
                DecomposeTRS(bones_t[bi], tt, rt, st);
                bones_b[bi] = ComposeTRS(
                    glm::mix(tf, tt, blend_alpha),
                    glm::slerp(rf, rt, blend_alpha),
                    glm::mix(sf, st, blend_alpha));
            }
            UploadBonesToSSBO_(mi, bones_b, kMaxBones);
        }
        DynamicDrawRequest req{};
        req.vao         = m.vao;
        req.vbo         = m.vbo;
        req.ebo         = m.ebo;
        req.index_count = m.idx_count;
        req.material_idx= m.material_idx;
        req.model       = model_matrix;
        req.bone_ssbo   = m.skinned ? bone_ssbos_[mi] : 0;
        req.bone_count  = kMaxBones;
        if (m.skinned) pipeline.SubmitSkinned(req);
        else           pipeline.SubmitDynamic(req);
    }
}

} // namespace rco::renderer
