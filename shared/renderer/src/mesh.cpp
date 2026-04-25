#include "rco/renderer/mesh.h"
#include <utility>
#include <new>

namespace rco::renderer {

Mesh::Mesh(const std::vector<Vertex>& vertices,
           const std::vector<uint32_t>& indices,
           const Material& mat)
    : vertexCount(indices.size()), material(mat) {
    glCreateBuffers(1, &vboID);
    glCreateBuffers(1, &eboID);
    glNamedBufferStorage(vboID, vertices.size() * sizeof(Vertex), vertices.data(), 0);
    glNamedBufferStorage(eboID, indices.size() * sizeof(uint32_t), indices.data(), 0);
}

Mesh::~Mesh() {
    glDeleteBuffers(1, &vboID);
    glDeleteBuffers(1, &eboID);
}

Mesh::Mesh(Mesh&& other) noexcept : material(other.material) {
    vboID       = std::exchange(other.vboID, 0);
    eboID       = std::exchange(other.eboID, 0);
    vertexCount = std::exchange(other.vertexCount, 0);
}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (&other == this) return *this;
    this->~Mesh();
    return *new(this) Mesh(std::move(other));
}

} // namespace rco::renderer
