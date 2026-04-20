#pragma once
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

namespace rco::renderer {

class Camera {
public:
    void Update(GLFWwindow* window, float dt);
    void ProcessScroll(float delta);

    glm::mat4 View()              const;
    glm::mat4 Projection(float aspect) const;
    glm::vec3 Position()          const;

    void      SetTarget(const glm::vec3& t) { target_ = t; }
    glm::vec3 Target()  const { return target_; }
    float     GetYaw()  const { return yaw_; }
    void      RotateYaw(float deg) { yaw_ += deg; }

    float fov   = 60.f;
    float znear = 0.5f;
    float zfar  = 2000.f;

private:
    glm::vec3 target_      = {0.f, 0.f, 0.f};
    float     yaw_         = 0.f;
    float     pitch_       = 50.f;  // isometric fixed pitch
    float     dist_        = 20.f;
};

} // namespace rco::renderer
