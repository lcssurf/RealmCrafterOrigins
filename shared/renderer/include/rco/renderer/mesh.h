#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <glad/glad.h>
#include "rco/renderer/utilities.h"
#include "rco/renderer/material.h"

namespace rco::renderer {

struct Vertex {
    glm::vec3 position {};
    glm::vec3 normal   {};
    glm::vec2 uv       {};
    glm::vec3 tangent  {};

    bool operator==(const Vertex& v) const {
        return position == v.position && normal == v.normal && uv == v.uv;
    }
};

class Mesh {
public:
    Mesh() = default;
    Mesh(const std::vector<Vertex>& vertices,
         const std::vector<uint32_t>& indices,
         const Material& mat);
    ~Mesh();

    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    unsigned GetVBOID()       const { return vboID; }
    unsigned GetEBOID()       const { return eboID; }
    size_t   GetVertexCount() const { return vertexCount; }
    const Material& GetMaterial() const { return material; }

private:
    unsigned vboID       {};
    unsigned eboID       {};
    size_t   vertexCount {};
    Material material    {};
};

struct MeshInfo {
    uint64_t    verticesAllocHandle {};
    uint64_t    indicesAllocHandle  {};
    std::string materialName        {};
    uint32_t    materialIndex       {};
};

} // namespace rco::renderer

namespace std {
    template<> struct hash<rco::renderer::Vertex> {
        std::size_t operator()(const rco::renderer::Vertex& v) const noexcept {
            std::size_t h1 = std::hash<glm::vec3>{}(v.position);
            std::size_t h2 = std::hash<glm::vec3>{}(v.normal);
            std::size_t h3 = std::hash<glm::vec2>{}(v.uv);
            std::size_t seed = 0;
            rco::renderer::hash_combine(seed, h1, h2, h3);
            return seed;
        }
    };
}
