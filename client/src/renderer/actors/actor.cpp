#include "renderer/actors/actor.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>

namespace rco::renderer {

void Actor::Init(const char* shader_dir, const char* model_path) {
    char vert[256], frag[256];
    std::snprintf(vert, sizeof(vert), "%s/actor.vert", shader_dir);
    std::snprintf(frag, sizeof(frag), "%s/actor.frag", shader_dir);
    if (!shader_.Load(vert, frag))
        std::fprintf(stderr, "[actor] Shader load failed\n");

    model_.Load(model_path);
}

void Actor::Render(const glm::mat4& view, const glm::mat4& proj,
                   const glm::vec3& cam_pos, const glm::vec3& sun_dir) {
    if (!model_.IsLoaded() && false) return; // always render (placeholder if no model)

    glm::mat4 m = glm::mat4(1.f);
    m = glm::translate(m, position);
    m = glm::rotate(m, glm::radians(yaw), {0.f, 1.f, 0.f});
    m = glm::scale(m, glm::vec3(scale));

    shader_.Use();
    shader_.SetMat4("uModel",    m);
    shader_.SetMat4("uView",     view);
    shader_.SetMat4("uProj",     proj);
    shader_.SetVec3("uCamPos",   cam_pos);
    shader_.SetVec3("uSunDir",   glm::normalize(sun_dir));
    shader_.SetVec3("uSunColor", {1.0f, 0.95f, 0.80f});
    shader_.SetVec3("uAmbient",  {0.20f, 0.22f, 0.28f});

    model_.Draw(shader_);
}

void Actor::Destroy() {
    model_.Destroy();
}

} // namespace rco::renderer
