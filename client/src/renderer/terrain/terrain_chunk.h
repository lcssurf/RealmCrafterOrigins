#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

namespace rco::renderer {

// One chunk of the terrain grid. Stores a static XZ grid mesh only —
// height is sampled from the heightmap GPU texture in the vertex shader,
// so the VBO never needs rebuilding after Init.
class TerrainChunk {
public:
    static constexpr int kSize = 64;

    TerrainChunk() = default;
    ~TerrainChunk();

    void Init(float world_x, float world_z, float cell_size = 1.f);
    void Draw() const;

    glm::vec3 Origin()    const { return origin_; }
    GLuint    vao()       const { return vao_; }
    GLuint    vbo()       const { return vbo_; }
    GLuint    ebo()       const { return ebo_; }
    int       idx_count() const { return idx_count_; }

private:
    void Upload();

    GLuint             vao_ = 0, vbo_ = 0, ebo_ = 0;
    glm::vec3          origin_    = {};
    float              cell_size_ = 1.f;
    std::vector<float>    vertices_;   // XZ world positions, 2 floats/vertex
    std::vector<uint32_t> indices_;
    int idx_count_ = 0;
};

} // namespace rco::renderer
