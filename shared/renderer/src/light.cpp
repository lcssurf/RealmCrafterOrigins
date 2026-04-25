#include "rco/renderer/light.h"
#include <cassert>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rco::renderer {

float PointLight::CalcRadiusSquared(float epsilon) const {
    assert(epsilon > 0.0f);
    float luminance = glm::max(diffuse.x, glm::max(diffuse.y, diffuse.z));
    float L = linear;
    float Q = quadratic;
    if (glm::epsilonEqual(Q, 0.f, .001f)) {
        return luminance / (L * epsilon);
    }
    float discriminant = glm::sqrt(L * L - 4 * (Q * (-1 / epsilon)));
    float root1 = (-L + discriminant) / (2 * Q);
    return luminance * glm::pow(root1, 2.0f);
}

glm::mat4 MakeLightMatrix(const DirLight& light, glm::vec3 eye,
                          glm::vec2 dim, glm::vec2 depthRange) {
    glm::mat4 lightView = glm::lookAt(eye, eye + light.direction, glm::vec3(0, 1, 0));
    glm::mat4 lightProj = glm::ortho(
        -dim.x / 2, dim.x / 2, -dim.y / 2, dim.y / 2,
        depthRange.x, depthRange.y);
    return lightProj * lightView;
}

} // namespace rco::renderer
