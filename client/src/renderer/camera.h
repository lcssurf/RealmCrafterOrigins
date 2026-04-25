#pragma once
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

namespace rco::renderer {

class Camera {
public:
    void Update(float dt);
    void ProcessScroll(float delta);

    // RMB drag feeds pixel deltas here to rotate yaw + pitch.
    void ApplyMouseDelta(float dx, float dy);

    // Called by A/D turning so the camera follows the character's facing.
    void SetYaw(float yaw)   { yaw_ = yaw; }
    void SetPitch(float p)   { pitch_ = p; }
    void AddYaw(float delta) { yaw_ += delta; }

    // Instantly snap smooth target (use after teleport / area change).
    void SnapTarget(const glm::vec3& t) { target_ = t; target_smooth_ = t; }
    void SetTarget(const glm::vec3& t);

    glm::mat4 View()                  const;
    glm::mat4 Projection(float aspect) const;
    glm::vec3 Position()              const;

    glm::vec3 Target() const { return target_smooth_; }
    float     GetYaw() const { return yaw_; }

    float fov   = 60.f;
    float znear = 0.5f;
    float zfar  = 1000.f;

private:
    glm::vec3 target_        = {};
    glm::vec3 target_smooth_ = {};
    float     yaw_           = 0.f;
    float     pitch_         = 15.f;   // RC: 0° is eye-level; we start slightly tilted

    // RC 1.26 ranges — allows looking up (negative pitch) and clamps zoom tight.
    static constexpr float kPitchMin = -70.f;
    static constexpr float kPitchMax =  85.f;
    static constexpr float kDistMin  =   3.f;
    static constexpr float kDistMax  =  25.f;
    float dist_ = 10.f;
};

} // namespace rco::renderer
