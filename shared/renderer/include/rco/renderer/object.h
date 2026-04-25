#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "rco/renderer/mesh.h"

namespace rco::renderer {

struct Transform {
    glm::vec3 translation { 0, 0, 0 };
    glm::quat rotation    { 1, 0, 0, 0 };
    glm::vec3 scale       { 1, 1, 1 };

    glm::mat4 GetModelMatrix() const {
        glm::mat4 m(1);
        m = glm::translate(m, translation);
        m *= mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
    glm::mat4 GetNormalMatrix() const {
        return glm::transpose(glm::inverse(glm::mat3(GetModelMatrix())));
    }
};

struct alignas(16) ObjectUniforms {
    glm::mat4 modelMatrix   {};
    uint32_t  materialIndex {};
};

struct ObjectBatched {
    Transform transform;
    std::vector<MeshInfo> meshes;
};

} // namespace rco::renderer
