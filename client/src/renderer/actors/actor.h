#pragma once
#include <glm/glm.hpp>
#include "renderer/model.h"
#include "renderer/shader.h"

namespace rco::renderer {

class Actor {
public:
    void Init(const char* shader_dir, const char* model_path);
    void Render(const glm::mat4& view, const glm::mat4& proj,
                const glm::vec3& cam_pos, const glm::vec3& sun_dir);
    void Destroy();

    glm::vec3 position = {0.f, 0.f, 0.f};
    float     yaw      = 0.f;   // degrees, Y-axis rotation
    float     scale    = 1.f;

private:
    Model  model_;
    Shader shader_;
};

} // namespace rco::renderer
