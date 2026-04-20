#include "renderer/camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace rco::renderer {

void Camera::ProcessScroll(float delta) {
    dist_ -= delta * 2.f;
    dist_ = std::clamp(dist_, 2.f, 80.f);
}

void Camera::Update(GLFWwindow* window, float dt) {
    // Q/E rotate camera left/right (isometric style).
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) yaw_ -= 90.f * dt;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) yaw_ += 90.f * dt;

    // +/- zoom.
    if (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS) dist_ -= 10.f * dt;
    if (glfwGetKey(window, GLFW_KEY_MINUS)  == GLFW_PRESS) dist_ += 10.f * dt;
    dist_ = std::clamp(dist_, 5.f, 80.f);

    (void)dt; // suppress unused warning when keys are not pressed
}

glm::mat4 Camera::View() const {
    float yr = glm::radians(yaw_);
    float pr = glm::radians(pitch_);
    glm::vec3 dir = {
        std::cos(pr) * std::sin(yr),
        std::sin(pr),
        std::cos(pr) * std::cos(yr)
    };
    glm::vec3 lookat = target_ + glm::vec3(0.f, 1.5f, 0.f);
    return glm::lookAt(lookat + dir * dist_, lookat, {0.f, 1.f, 0.f});
}

glm::mat4 Camera::Projection(float aspect) const {
    return glm::perspective(glm::radians(fov), aspect, znear, zfar);
}

glm::vec3 Camera::Position() const {
    float yr = glm::radians(yaw_);
    float pr = glm::radians(pitch_);
    glm::vec3 dir = {
        std::cos(pr) * std::sin(yr),
        std::sin(pr),
        std::cos(pr) * std::cos(yr)
    };
    return target_ + glm::vec3(0.f, 1.5f, 0.f) + dir * dist_;
}

} // namespace rco::renderer
