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
    dist_target_ -= delta * 2.f;
    dist_target_  = std::clamp(dist_target_, kDistMin, kDistMax);
}

void Camera::ApplyMouseDelta(float dx, float dy) {
    yaw_   += dx * 0.25f;
    pitch_ -= dy * 0.25f;
    pitch_  = std::clamp(pitch_, kPitchMin, kPitchMax);
}

void Camera::Update(float dt) {
    // Responsive target follow (20 hz effective responsiveness).
    float lag      = std::min(1.f, 20.f * dt);
    target_smooth_ = glm::mix(target_smooth_, target_, lag);

    // Smooth zoom — dist_ catches up to dist_target_ quickly.
    dist_ = glm::mix(dist_, dist_target_, std::min(1.f, 15.f * dt));

    dist_  = std::clamp(dist_,  kDistMin,  kDistMax);
    pitch_ = std::clamp(pitch_, kPitchMin, kPitchMax);
}

void Camera::LerpYawToward(float target_deg, float speed, float dt) {
    // Shortest-arc yaw interpolation to avoid 360° wrap artefacts.
    float diff = target_deg - yaw_;
    // Wrap diff into [-180, 180]
    while (diff >  180.f) diff -= 360.f;
    while (diff < -180.f) diff += 360.f;
    float step = speed * dt;
    if (std::abs(diff) <= step)
        yaw_ = target_deg;
    else
        yaw_ += (diff > 0.f ? step : -step);
}

glm::mat4 Camera::View() const {
    float yr = glm::radians(yaw_);
    float pr = glm::radians(pitch_);
    glm::vec3 dir    = { std::cos(pr) * std::sin(yr),
                         std::sin(pr),
                         std::cos(pr) * std::cos(yr) };
    glm::vec3 pivot  = target_smooth_ + glm::vec3(0.f, pivot_h_,  0.f);
    glm::vec3 lookat = target_smooth_ + glm::vec3(0.f, lookat_h_, 0.f);
    return glm::lookAt(pivot + dir * dist_, lookat, {0.f, 1.f, 0.f});
}

glm::mat4 Camera::Projection(float aspect) const {
    return glm::perspective(glm::radians(fov), aspect, znear, zfar);
}

glm::vec3 Camera::Position() const {
    float yr = glm::radians(yaw_);
    float pr = glm::radians(pitch_);
    glm::vec3 dir   = { std::cos(pr) * std::sin(yr),
                        std::sin(pr),
                        std::cos(pr) * std::cos(yr) };
    glm::vec3 pivot = target_smooth_ + glm::vec3(0.f, pivot_h_, 0.f);
    return pivot + dir * dist_;
}

} // namespace rco::renderer
