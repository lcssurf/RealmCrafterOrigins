#include "renderer/actors/actor.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>

namespace rco::renderer {

// ---------------------------------------------------------------------------
// Init / LoadAnim / Destroy
// ---------------------------------------------------------------------------

void Actor::Init(const char* shader_dir, const char* model_path) {
    char vert[256], frag[256];
    std::snprintf(vert, sizeof(vert), "%s/actor.vert", shader_dir);
    std::snprintf(frag, sizeof(frag), "%s/actor.frag", shader_dir);
    if (!shader_.Load(vert, frag)) {
        std::fprintf(stderr, "[actor] Shader load failed\n");
        return;
    }
    shader_.Use();
    shader_.SetInt("uAlbedo",    0);
    shader_.SetInt("uNormalMap", 1);
    shader_.SetInt("uORM",       2);

    bone_mats_.fill(glm::mat4(1.f));
    model_.Load(model_path);

    // Auto-play "Idle" if the model has it, otherwise clip 0.
    if (model_.HasAnimations())
        PlayAnim("Idle", true);
}

void Actor::LoadAnim(const char* path, const char* name) {
    model_.AppendAnimationsFrom(path, name);
}

void Actor::Destroy() {
    model_.Destroy();
}

// ---------------------------------------------------------------------------
// Clip lookup
// ---------------------------------------------------------------------------

int Actor::FindClip(const std::string& name) const {
    // Exact match first.
    for (int i = 0; i < model_.ClipCount(); ++i)
        if (model_.ClipName(i) == name) return i;
    // Case-insensitive prefix match fallback.
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
        // Unknown clip — fall back to clip 0 (better than nothing).
        if (model_.ClipCount() > 0) idx = 0;
        else return;
    }

    if (idx == clip_idx_ && !restart) {
        // Same clip — just update loop/return_to without rewinding.
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

    // One-shot end detection.
    if (!loop_) {
        // ComputeBones uses fmod internally, but we need raw duration here.
        // Access via ClipName is not enough — expose duration via a helper,
        // or clamp manually. For simplicity: after 2× duration, transition.
        // (A proper fix would expose clip duration — added below.)
        float dur = model_.ClipDuration(clip_idx_);
        if (dur > 0.f && anim_t_ >= dur) {
            anim_t_ = dur; // hold last frame
            if (!return_to_.empty()) {
                PlayAnim(return_to_, true);
                return;
            }
        }
    }

    model_.ComputeBones(clip_idx_, anim_t_, bone_mats_.data(), kMaxBones);
}

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------

void Actor::SetupModelUniforms(const glm::mat4& view, const glm::mat4& proj,
                               const glm::vec3& cam_pos, const glm::vec3& sun_dir) {
    glm::mat4 m = glm::mat4(1.f);
    m = glm::translate(m, position);
    m = glm::rotate(m, glm::radians(yaw), {0.f, 1.f, 0.f});
    m = glm::scale(m, glm::vec3(scale));

    shader_.Use();
    shader_.SetMat4("uModel",    m);
    shader_.SetMat4("uView",     view);
    shader_.SetMat4("uProj",     proj);
    shader_.SetVec3("uCamPos",   cam_pos);
    shader_.SetVec3("uSunDir",   glm::normalize(sun_dir));
    shader_.SetVec3("uSunColor", {1.0f, 0.95f, 0.80f});
    shader_.SetVec3("uAmbient",  {0.20f, 0.22f, 0.28f});
}

void Actor::UploadBones() {
    if (!model_.HasAnimations() || clip_idx_ < 0) return;
    GLint loc = glGetUniformLocation(shader_.id(), "uBones[0]");
    if (loc >= 0)
        glUniformMatrix4fv(loc, kMaxBones, GL_FALSE,
                           glm::value_ptr(bone_mats_[0]));
}

void Actor::Render(const glm::mat4& view, const glm::mat4& proj,
                   const glm::vec3& cam_pos, const glm::vec3& sun_dir) {
    SetupModelUniforms(view, proj, cam_pos, sun_dir);
    UploadBones();
    model_.Draw(shader_);
}

// ---------------------------------------------------------------------------
// RenderAs — external state/time (world actors sharing this instance)
// ---------------------------------------------------------------------------

void Actor::RenderAs(const std::string& anim_name, float anim_t, bool loop,
                     const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& cam_pos, const glm::vec3& sun_dir) {
    SetupModelUniforms(view, proj, cam_pos, sun_dir);

    if (model_.HasAnimations()) {
        int cidx = FindClip(anim_name);
        if (cidx < 0 && model_.ClipCount() > 0) cidx = 0;

        if (cidx >= 0) {
            float t = anim_t;
            if (loop) {
                float dur = model_.ClipDuration(cidx);
                if (dur > 0.f) t = std::fmod(anim_t, dur);
            } else {
                float dur = model_.ClipDuration(cidx);
                if (dur > 0.f && t > dur) t = dur;
            }
            std::array<glm::mat4, kMaxBones> tmp;
            tmp.fill(glm::mat4(1.f));
            model_.ComputeBones(cidx, t, tmp.data(), kMaxBones);
            GLint loc = glGetUniformLocation(shader_.id(), "uBones[0]");
            if (loc >= 0)
                glUniformMatrix4fv(loc, kMaxBones, GL_FALSE,
                                   glm::value_ptr(tmp[0]));
        }
    }

    model_.Draw(shader_);
}

} // namespace rco::renderer
