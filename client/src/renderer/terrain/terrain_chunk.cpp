#include "renderer/terrain/terrain_chunk.h"
#include <glm/glm.hpp>
#include <algorithm>

namespace rco::renderer {

TerrainChunk::~TerrainChunk() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (ebo_) glDeleteBuffers(1, &ebo_);
}

float TerrainChunk::SampleH(int x, int z) const {
    x = std::clamp(x, 0, kSize - 1);
    z = std::clamp(z, 0, kSize - 1);
    return heights_[z * kSize + x];
}

void TerrainChunk::Init(float wx, float wz, float cs) {
    origin_    = {wx, 0.f, wz};
    cell_size_ = cs;
    heights_.assign(kSize * kSize, 0.f);

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

    dirty_ = true;
    Upload();
}

void TerrainChunk::SetHeights(const std::vector<float>& h) {
    if (static_cast<int>(h.size()) != kSize * kSize) return;
    heights_ = h;
    dirty_   = true;
    Upload();
}

void TerrainChunk::Upload() {
    if (!dirty_ || !vao_) return;
    dirty_ = false;

    vertices_.clear();
    vertices_.reserve(kSize * kSize * 6);

    for (int z = 0; z < kSize; ++z) {
        for (int x = 0; x < kSize; ++x) {
            float h  = SampleH(x, z);
            float wx = origin_.x + x * cell_size_;
            float wz = origin_.z + z * cell_size_;

            float hr = SampleH(x + 1, z);
            float hl = SampleH(x - 1, z);
            float hu = SampleH(x, z + 1);
            float hd = SampleH(x, z - 1);
            glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.f * cell_size_, hd - hu));

            vertices_.insert(vertices_.end(), {wx, h, wz, n.x, n.y, n.z});
        }
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
        vertices_.size() * sizeof(float), vertices_.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void TerrainChunk::Draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, idx_count_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace rco::renderer
