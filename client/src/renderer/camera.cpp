#include "renderer/camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace rco::renderer {

void Camera::SetTarget(const glm::vec3& t) {
    // Snap on large jumps (first frame or teleport) to avoid lerping from origin.
    if (glm::length(t - target_smooth_) > 50.f)
        target_smooth_ = t;
    target_ = t;
}

void Camera::ProcessScroll(float delta) {
    dist_ -= delta * 2.f;
    dist_  = std::clamp(dist_, kDistMin, kDistMax);
}

void Camera::ApplyMouseDelta(float dx, float dy) {
    yaw_   += dx * 0.25f;
    pitch_ -= dy * 0.25f;
    pitch_  = std::clamp(pitch_, kPitchMin, kPitchMax);
}

void Camera::Update(float dt) {
    // Smooth / lazy target follow — camera slightly lags the player.
    float lag      = std::min(1.f, 10.f * dt);
    target_smooth_ = glm::mix(target_smooth_, target_, lag);

    dist_  = std::clamp(dist_, kDistMin, kDistMax);
    pitch_ = std::clamp(pitch_, kPitchMin, kPitchMax);
}

glm::mat4 Camera::View() const {
    float yr = glm::radians(yaw_);
    float pr = glm::radians(pitch_);
    glm::vec3 dir    = { std::cos(pr) * std::sin(yr),
                         std::sin(pr),
                         std::cos(pr) * std::cos(yr) };
    glm::vec3 lookat = target_smooth_ + glm::vec3(0.f, 1.5f, 0.f);
    return glm::lookAt(lookat + dir * dist_, lookat, {0.f, 1.f, 0.f});
}

glm::mat4 Camera::Projection(float aspect) const {
    return glm::perspective(glm::radians(fov), aspect, znear, zfar);
}

glm::vec3 Camera::Position() const {
    float yr = glm::radians(yaw_);
    float pr = glm::radians(pitch_);
    glm::vec3 dir = { std::cos(pr) * std::sin(yr),
                      std::sin(pr),
                      std::cos(pr) * std::cos(yr) };
    return target_smooth_ + glm::vec3(0.f, 1.5f, 0.f) + dir * dist_;
}

} // namespace rco::renderer
