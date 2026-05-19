#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace gue {

// Free-fly camera for the zone editor viewport.
// All input is provided explicitly per-frame (no GLFW dependency) so it
// integrates cleanly with ImGui's per-frame input polling.
struct ZoneCamera {
    glm::vec3 pos   = {64.f, 30.f, 64.f};
    float     yaw   = 0.f;   // degrees, 0 = looking +Z
    float     pitch = 25.f;  // degrees, positive = looking down
    float     speed = 20.f;  // units/s — exposed in the bottom bar
    float     sens  = 0.20f; // mouse sensitivity (deg/px)

    static constexpr float kFovY  = 60.f;
    static constexpr float kNearZ = 0.5f;
    static constexpr float kFarZ  = 4000.f;

    glm::vec3 Forward() const {
        float y = glm::radians(yaw);
        float p = glm::radians(pitch);
        return glm::normalize(glm::vec3(
             std::sin(y) * std::cos(p),
            -std::sin(p),
             std::cos(y) * std::cos(p)
        ));
    }

    glm::vec3 Right() const {
        return glm::normalize(glm::cross(Forward(), glm::vec3(0.f, 1.f, 0.f)));
    }

    glm::mat4 View() const {
        return glm::lookAt(pos, pos + Forward(), glm::vec3(0.f, 1.f, 0.f));
    }

    glm::mat4 Proj(float aspect) const {
        return glm::perspective(glm::radians(kFovY), aspect, kNearZ, kFarZ);
    }

    // Update camera position and orientation for one frame.
    // fwd/back/left/right/up/down: movement direction booleans from key state.
    // mouseDX/DY: cursor delta in screen pixels (only applied when mouselook is
    // active — the caller is responsible for gating these on RMB state).
    void Update(float dt,
                bool fwd, bool back, bool left, bool right, bool up, bool down,
                float mouseDX, float mouseDY) {
        const glm::vec3 f = Forward();
        const glm::vec3 r = Right();
        const glm::vec3 u = glm::vec3(0.f, 1.f, 0.f);
        if (fwd)   pos += f * speed * dt;
        if (back)  pos -= f * speed * dt;
        if (left)  pos -= r * speed * dt;
        if (right) pos += r * speed * dt;
        if (up)    pos += u * speed * dt;
        if (down)  pos -= u * speed * dt;

        // Unreal-style feel: moving mouse right yaws camera to the right.
        yaw   -= mouseDX * sens;
        pitch += mouseDY * sens;
        pitch  = std::clamp(pitch, -89.f, 89.f);
    }

    // Build a normalised world-space ray from NDC coordinates (both in [-1, 1]).
    // Pass (mouseX - vpLeft) / vpWidth * 2 - 1  for ndcX, etc.
    glm::vec3 NDCRay(float ndcX, float ndcY, float aspect) const {
        glm::mat4 invVP = glm::inverse(Proj(aspect) * View());
        glm::vec4 n4 = invVP * glm::vec4(ndcX, ndcY, -1.f, 1.f);
        glm::vec4 f4 = invVP * glm::vec4(ndcX, ndcY,  1.f, 1.f);
        glm::vec3 np = glm::vec3(n4) / n4.w;
        glm::vec3 fp = glm::vec3(f4) / f4.w;
        return glm::normalize(fp - np);
    }
};

} // namespace gue
