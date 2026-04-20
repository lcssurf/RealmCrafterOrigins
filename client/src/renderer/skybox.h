#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "renderer/shader.h"

namespace rco::renderer {

class Skybox {
public:
    bool Init(const char* shader_dir);
    void Render(const glm::mat4& view, const glm::mat4& proj);
    void Destroy();

    glm::vec3 sun_dir     = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
    glm::vec3 sky_top     = {0.18f, 0.36f, 0.72f};
    glm::vec3 sky_horizon = {0.60f, 0.75f, 0.90f};
    glm::vec3 sun_color   = {1.0f,  0.95f, 0.80f};

private:
    Shader shader_;
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
};

} // namespace rco::renderer
