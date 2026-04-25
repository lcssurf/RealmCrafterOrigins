#pragma once
#include <glm/glm.hpp>

namespace rco::renderer {

struct PointLight {
    glm::vec4 diffuse       {};
    glm::vec4 position      {};
    float     linear        {};
    float     quadratic     {};
    float     radiusSquared {};
    float     _padding      {};

    float CalcRadiusSquared(float epsilon) const;
};

struct DirLight {
    glm::vec3 diffuse   {};
    glm::vec3 direction {};
};

glm::mat4 MakeLightMatrix(const DirLight& light, glm::vec3 eye,
                          glm::vec2 dim, glm::vec2 depthRange);

} // namespace rco::renderer
