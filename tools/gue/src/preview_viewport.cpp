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

namespace gue {

PreviewViewport::~PreviewViewport() {
    actor_.Destroy();
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

bool PreviewViewport::LoadModel(const std::string& path) {
    if (path == current_path_) return true;

    actor_.Destroy();
    current_path_ = path;
    anim_t_ = 0.f;

    if (path.empty()) return true;

    std::string resolved = ResolveClientAsset(path);
    rco::renderer::MaterialManager* mm = engine_ ? &engine_->materials() : nullptr;
    actor_.Init("", resolved.c_str(), mm);
    if (engine_) engine_->RebuildMaterialsBuffer();

    // Actor::Init auto-starts "Idle" when the model has animations. If the
    // model ships with clips under another name, fall back to the first one.
    if (actor_.CurrentAnim().empty() && actor_.model().ClipCount() > 0)
        actor_.PlayAnim(actor_.model().ClipName(0), true);

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
    engine_->RebuildMaterialsBuffer();
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

    int engW = engine_->width();
    int engH = engine_->height();
    if (engW <= 0 || engH <= 0) return;

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
                                      (float)engW / (float)engH, 0.05f, 200.0f);

    pipeline_->Begin(view, proj, eye, dt);
    pipeline_->SetSun(glm::vec3(-0.4f, -1.0f, -0.3f), glm::vec3(1.0f, 0.95f, 0.80f));

    if (actor_.IsLoaded()) {
        if (playing_) {
            anim_t_ += dt;
            actor_.SubmitAs(actor_.CurrentAnim(), anim_t_, /*loop*/true, *pipeline_);
        } else {
            // Frozen pose — reuse the current anim time.
            actor_.SubmitAs(actor_.CurrentAnim(), anim_t_, /*loop*/true, *pipeline_);
        }
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
            cam_dist_ = std::clamp(cam_dist_, 0.2f, 50.f);
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
    ImGui::Image(
        (ImTextureID)(intptr_t)engine_->finalImage(),
        view_size,
        ImVec2(0.f, 1.f), ImVec2(1.f, 0.f)
    );

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
    if (ImGui::Button("Reset cam")) {
        cam_yaw_   = 0.f;
        cam_pitch_ = -15.f;
        cam_dist_  = 2.5f;
    }
}

} // namespace gue
