#include "preview_viewport.h"
#include "asset_path.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <unordered_map>

#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"
#include "rco/renderer/shader.h"
#include "rco/renderer/model_cache.h"

namespace gue {

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
    anim_t_ = 0.f;
}

void PreviewViewport::FitCameraToModel() {
    const auto& mdl = actor_.model();
    if (!mdl.IsLoaded()) return;

    glm::vec3 bmin = mdl.BoundsMin();
    glm::vec3 bmax = mdl.BoundsMax();

    // Guard against degenerate / empty AABB (placeholder box or no verts).
    if (bmin.x > bmax.x) {
        bmin = glm::vec3(-0.5f, 0.f, -0.5f);
        bmax = glm::vec3( 0.5f, 1.f,  0.5f);
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
    if (path == current_path_) return true;

    actor_.Destroy();
    current_path_ = path;
    anim_t_ = 0.f;

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
                                       float roughness, float metallic) {
    if (!actor_.IsLoaded()) return;
    actor_.OverrideMaterial(
        ResolveClientAsset(albedo),
        ResolveClientAsset(normal),
        ResolveClientAsset(orm),
        ar, ag, ab, roughness, metallic);
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
    // every driver without GL_ARB_bindless_texture involvement.
    const bool is_static = actor_.IsLoaded() && !actor_.model().HasAnimations()
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
        sh->SetMat4("u_model",    glm::mat4(1.0f));  // actor at origin, prebaked
        sh->SetVec2("u_uvOffset", uv_offset_[0], uv_offset_[1]);
        sh->SetVec2("u_uvScale",  uv_scale_[0],  uv_scale_[1]);
        sh->SetVec3("u_sunDir",   glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)));
        sh->SetVec3("u_sunColor", glm::vec3(1.0f, 0.95f, 0.80f) * sun_intensity_);
        sh->SetFloat("u_ambientStrength", 0.25f);

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
        anim_t_ += playing_ ? dt : 0.f;
        actor_.SubmitAs(actor_.CurrentAnim(), anim_t_, /*loop*/true, *pipeline_);
    }

    rco::renderer::Pipeline::EndConfig cfg{};
    cfg.blit_to_default = false;
    pipeline_->End(cfg);
}

void PreviewViewport::DrawImGui() {
    if (!engine_ || !pipeline_) {
        ImGui::TextDisabled("Preview unavailable (engine/pipeline not initialized).");
        return;
    }

    ImVec2 region = ImGui::GetContentRegionAvail();
    if (region.x < 64.f) region.x = 64.f;
    if (region.y < 64.f) region.y = 64.f;

    const float controls_h = 28.f;
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

    const auto& mdl = actor_.model();
    if (mdl.HasAnimations() && mdl.ClipCount() > 0) {
        ImGui::Checkbox("Play", &playing_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        const std::string& cur = actor_.CurrentAnim();
        if (ImGui::BeginCombo("##clip", cur.empty() ? "(none)" : cur.c_str())) {
            for (int i = 0; i < mdl.ClipCount(); ++i) {
                bool sel = (mdl.ClipName(i) == cur);
                if (ImGui::Selectable(mdl.ClipName(i).c_str(), sel)) {
                    actor_.PlayAnim(mdl.ClipName(i), true);
                    anim_t_ = 0.f;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Reset cam")) FitCameraToModel();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.f);
    ImGui::SliderFloat("Light", &sun_intensity_, 0.0f, 4.0f, "%.2f");

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
