#include "rco/renderer/actor.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cmath>

#include "rco/renderer/pipeline.h"

namespace rco::renderer {

// ---------------------------------------------------------------------------
// Init / LoadAnim / Destroy
// ---------------------------------------------------------------------------

void Actor::Init(const char* /*shader_dir*/, const char* model_path,
                 MaterialManager* mm) {
    bool loaded = model_.Load(model_path, mm);
    std::fprintf(stderr,
        "[actor] Init done: model='%s' loaded=%d has_anims=%d clips=%d\n",
        model_path, loaded ? 1 : 0,
        model_.HasAnimations() ? 1 : 0, model_.ClipCount());

    if (model_.HasAnimations())
        PlayAnim("Idle", true);
}

void Actor::LoadAnim(const char* path, const char* name) {
    model_.AppendAnimationsFrom(path, name);
}

void Actor::Destroy() {
    for (GLuint& id : bone_ssbos_) {
        if (id) { glDeleteBuffers(1, &id); id = 0; }
    }
    bone_ssbos_.clear();
    model_.Destroy();
}

// ---------------------------------------------------------------------------
// Clip lookup
// ---------------------------------------------------------------------------

int Actor::FindClip(const std::string& name) const {
    for (int i = 0; i < model_.ClipCount(); ++i)
        if (model_.ClipName(i) == name) return i;
    for (int i = 0; i < model_.ClipCount(); ++i) {
        const std::string& cn = model_.ClipName(i);
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
        if (model_.ClipCount() > 0) idx = 0;
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
    if (!model_.HasAnimations() || clip_idx_ < 0) return;

    anim_t_ += dt;

    if (!loop_) {
        float dur = model_.ClipDuration(clip_idx_);
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
    if (!model_.IsLoaded()) return;

    const auto& meshes = model_.meshes();
    EnsureBoneSSBOs_(meshes.size());

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, position);
    model = glm::rotate(model, glm::radians(yaw), glm::vec3(0.f, 1.f, 0.f));
    model = glm::scale(model, glm::vec3(scale));

    glm::mat4 bones[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        if (m.skinned) {
            model_.ComputeBones(clip_idx_, anim_t_, (int)mi, bones, kMaxBones);
            UploadBonesToSSBO_(mi, bones, kMaxBones);
        }
        DynamicDrawRequest req{};
        req.vao          = m.vao;
        req.vbo          = m.vbo;
        req.ebo          = m.ebo;
        req.index_count  = m.idx_count;
        req.material_idx = m.material_idx;
        req.model        = model;
        req.bone_ssbo    = m.skinned ? bone_ssbos_[mi] : 0;
        req.bone_count   = kMaxBones;
        if (m.skinned) pipeline.SubmitSkinned(req);
        else           pipeline.SubmitDynamic(req);
    }
}

void Actor::SubmitWithMatrix(Pipeline& pipeline, const glm::mat4& model_matrix) {
    if (!model_.IsLoaded()) return;

    const auto& meshes = model_.meshes();
    EnsureBoneSSBOs_(meshes.size());

    glm::mat4 bones[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        if (m.skinned) {
            model_.ComputeBones(clip_idx_, anim_t_, (int)mi, bones, kMaxBones);
            UploadBonesToSSBO_(mi, bones, kMaxBones);
        }
        DynamicDrawRequest req{};
        req.vao          = m.vao;
        req.vbo          = m.vbo;
        req.ebo          = m.ebo;
        req.index_count  = m.idx_count;
        req.material_idx = m.material_idx;
        req.model        = model_matrix;
        req.bone_ssbo    = m.skinned ? bone_ssbos_[mi] : 0;
        req.bone_count   = kMaxBones;
        if (m.skinned) pipeline.SubmitSkinned(req);
        else           pipeline.SubmitDynamic(req);
    }
}

void Actor::SubmitAs(const std::string& anim_name, float anim_t, bool loop,
                     Pipeline& pipeline) {
    if (!model_.IsLoaded()) return;

    const auto& meshes = model_.meshes();
    EnsureBoneSSBOs_(meshes.size());

    // Resolve target clip once
    int cidx = -1;
    float t = 0.f;
    if (model_.HasAnimations()) {
        cidx = FindClip(anim_name);
        if (cidx < 0 && model_.ClipCount() > 0) cidx = 0;
        if (cidx >= 0) {
            float dur = model_.ClipDuration(cidx);
            t = anim_t;
            if (loop) { if (dur > 0.f) t = std::fmod(anim_t, dur); }
            else      { if (dur > 0.f && t > dur) t = dur; }
        }
    }

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, position);
    model = glm::rotate(model, glm::radians(yaw), glm::vec3(0.f, 1.f, 0.f));
    model = glm::scale(model, glm::vec3(scale));

    glm::mat4 bones[kMaxBones];
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        if (m.skinned) {
            model_.ComputeBones(cidx, t, (int)mi, bones, kMaxBones);
            UploadBonesToSSBO_(mi, bones, kMaxBones);
        }
        DynamicDrawRequest req{};
        req.vao          = m.vao;
        req.vbo          = m.vbo;
        req.ebo          = m.ebo;
        req.index_count  = m.idx_count;
        req.material_idx = m.material_idx;
        req.model        = model;
        req.bone_ssbo    = m.skinned ? bone_ssbos_[mi] : 0;
        req.bone_count   = kMaxBones;
        if (m.skinned) pipeline.SubmitSkinned(req);
        else           pipeline.SubmitDynamic(req);
    }
}

} // namespace rco::renderer
