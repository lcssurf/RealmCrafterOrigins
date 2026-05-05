#include "renderer/terrain/terrain_chunk.h"
#include <glm/glm.hpp>

namespace rco::renderer {

TerrainChunk::~TerrainChunk() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (ebo_) glDeleteBuffers(1, &ebo_);
}

void TerrainChunk::Init(float wx, float wz, float cs) {
    origin_    = {wx, 0.f, wz};
    cell_size_ = cs;

    indices_.clear();
    for (int z = 0; z < kSize - 1; ++z)
        for (int x = 0; x < kSize - 1; ++x) {
            uint32_t tl = z * kSize + x;
            uint32_t tr = tl + 1;
            uint32_t bl = tl + kSize;
            uint32_t br = bl + 1;
            indices_.insert(indices_.end(), {tl, bl, tr, tr, bl, br});
        }
    idx_count_ = static_cast<int>(indices_.size());

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        indices_.size() * sizeof(uint32_t), indices_.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);

    Upload();
}

void TerrainChunk::Upload() {
    vertices_.clear();
    vertices_.reserve(kSize * kSize * 2);

    for (int z = 0; z < kSize; ++z) {
        for (int x = 0; x < kSize; ++x) {
            vertices_.push_back(origin_.x + x * cell_size_);  // world X
            vertices_.push_back(origin_.z + z * cell_size_);  // world Z
        }
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
        vertices_.size() * sizeof(float), vertices_.data(), GL_STATIC_DRAW);
    // a_xz: vec2 world-space XZ
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void TerrainChunk::Draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, idx_count_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace rco::renderer
