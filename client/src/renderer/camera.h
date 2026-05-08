#pragma once
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

namespace rco::renderer {

class Camera {
public:
    void Update(float dt);
    void ProcessScroll(float delta);

    // Mouse drag feeds pixel deltas here to rotate yaw + pitch.
    void ApplyMouseDelta(float dx, float dy);

    // Smooth yaw toward target angle (handles 360° wrap). speed is units/s.
    void LerpYawToward(float target_deg, float speed, float dt);

    // Called by A/D turning so the camera follows the character's facing.
    void SetYaw(float yaw)   { yaw_ = yaw; }
    void SetPitch(float p)   { pitch_ = p; }
    void AddYaw(float delta) { yaw_ += delta; }

    // Derive lookat height from the model's world-space height.
    // Call once after the player model is initialized (or when it changes).
    void SetActorHeight(float h) {
        lookat_h_ = h * 1.05f;
    }

    // Instantly snap smooth target (use after teleport / area change).
    void SnapTarget(const glm::vec3& t) { target_ = t; target_smooth_ = t; }
    void SetTarget(const glm::vec3& t);

    glm::mat4 View()                  const;
    glm::mat4 Projection(float aspect) const;
    glm::vec3 Position()              const;

    glm::vec3 Target()    const { return target_smooth_; }
    float     GetYaw()   const { return yaw_; }
    float     GetPitch() const { return pitch_; }

    // Called each frame by main.cpp after the terrain-collision march.
    // Constrains the effective camera distance so the lens never clips into terrain.
    void SetCollisionDist(float d) { dist_collision_ = d; }

    float fov           = 70.f;
    float znear         = 0.5f;
    float zfar          = 1000.f;
    float default_pitch = 32.f;  // pitch restaurado ao andar (classic mode)

    // When true, View() shifts the lookat/pivot laterally so the player model
    // sits slightly left of center (crosshair-mode shoulder offset).
    bool  action_mode   = false;
    float action_offset = 0.8f;  // world-units of right-strafe offset

private:
    glm::vec3 target_        = {};
    glm::vec3 target_smooth_ = {};
    float     yaw_           = 0.f;
    float     pitch_         = default_pitch;

    static constexpr float kPitchMin = -70.f;
    static constexpr float kPitchMax =  85.f;
    static constexpr float kDistMin  =   3.f;
    static constexpr float kDistMax  =  35.f;
    float dist_           = 14.f;
    float dist_target_    = 14.f;
    float dist_collision_ = kDistMax;
    float lookat_h_       = 2.0f;
};

} // namespace rco::renderer
