#include "fx_preview_viewport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/gtc/matrix_transform.hpp>

namespace gue {

void FXPreviewViewport::Shutdown() {
    if (particles_init_) {
        particles_.Shutdown();
        particles_init_ = false;
    }
    if (fbo_) { glDeleteFramebuffers(1, &fbo_);   fbo_ = 0; }
    if (color_) { glDeleteTextures(1, &color_);  color_ = 0; }
    if (depth_) { glDeleteRenderbuffers(1, &depth_); depth_ = 0; }
    w_ = h_ = 0;
}

void FXPreviewViewport::SetParams(const rco::renderer::FXParams& params) {
    params_ = params;
    has_params_ = true;
    last_spawn_time_ = -1e9f;
    spawn_period_ = std::max(0.6f, params.lifetimeSeconds * 1.4f);
}

void FXPreviewViewport::EnsureFbo_(int w, int h) {
    if (w == w_ && h == h_ && fbo_) return;

    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);

    glGenTextures(1, &color_);
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          color_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             GL_RENDERBUFFER, depth_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    w_ = w;
    h_ = h;
}

glm::mat4 FXPreviewViewport::ViewMatrix_() const {
    float cp = std::cos(cam_pitch_);
    float sp = std::sin(cam_pitch_);
    float cy = std::cos(cam_yaw_);
    float sy = std::sin(cam_yaw_);

    glm::vec3 dir = {cp * sy, sp, cp * cy};
    glm::vec3 eye = cam_target_ - dir * cam_dist_;
    return glm::lookAt(eye, cam_target_, glm::vec3(0.f, 1.f, 0.f));
}

glm::mat4 FXPreviewViewport::ProjMatrix_(float aspect) const {
    return glm::perspective(glm::radians(55.f), aspect, 0.1f, 200.f);
}

void FXPreviewViewport::HandleOrbitInput_(ImVec2 imageMin, ImVec2 imageSize) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mp = io.MousePos;
    bool hovered = mp.x >= imageMin.x && mp.x <= imageMin.x + imageSize.x &&
                   mp.y >= imageMin.y && mp.y <= imageMin.y + imageSize.y;
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        cam_yaw_ -= io.MouseDelta.x * 0.01f;
        cam_pitch_ += io.MouseDelta.y * 0.01f;
        const float lim = 1.5f;
        if (cam_pitch_ > lim) cam_pitch_ = lim;
        if (cam_pitch_ < -lim) cam_pitch_ = -lim;
    }

    if (hovered && io.MouseWheel != 0.f) {
        cam_dist_ -= io.MouseWheel * 1.0f;
        if (cam_dist_ < 2.f) cam_dist_ = 2.f;
        if (cam_dist_ > 60.f) cam_dist_ = 60.f;
    }
}

void FXPreviewViewport::Draw(ImVec2 size) {
    int w = std::max(16, static_cast<int>(size.x));
    int h = std::max(16, static_cast<int>(size.y));

    if (!particles_init_) {
        particles_.Init();
        particles_init_ = true;
    }
    EnsureFbo_(w, h);

    float now = static_cast<float>(ImGui::GetTime());
    float dt = ImGui::GetIO().DeltaTime;

    if (has_params_ && (now - last_spawn_time_) >= spawn_period_) {
        rco::renderer::FXParams p = params_;
        float duration = (p.burstCount > 0) ? 0.f : std::max(0.4f, p.lifetimeSeconds);
        particles_.SpawnEmitterParams(p, {0.f, 1.f, 0.f}, now, duration);
        last_spawn_time_ = now;
    }

    particles_.Update(now, dt);

    GLint prevFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevVp[4];
    glGetIntegerv(GL_VIEWPORT, prevVp);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w_, h_);
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    float aspect = (h_ > 0) ? (float)w_ / (float)h_ : 1.f;
    particles_.Render(ViewMatrix_(), ProjMatrix_(aspect));

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);

    ImVec2 imageMin = ImGui::GetCursorScreenPos();
    ImTextureID img_id = (ImTextureID)(intptr_t)color_;
    ImGui::Image(img_id, size, ImVec2(0, 1), ImVec2(1, 0));
    HandleOrbitInput_(imageMin, size);
}

} // namespace gue
