#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

namespace rco::renderer {

class TerrainChunk {
public:
    static constexpr int kSize = 64;

    TerrainChunk() = default;
    ~TerrainChunk();

    void Init(float world_x, float world_z, float cell_size = 1.f);
    void SetHeights(const std::vector<float>& h);
    void Draw() const;

    glm::vec3 Origin() const { return origin_; }

private:
    void Upload();
    float SampleH(int x, int z) const;

    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    glm::vec3             origin_    = {};
    float                 cell_size_ = 1.f;
    std::vector<float>    heights_;
    std::vector<float>    vertices_;
    std::vector<uint32_t> indices_;
    int  idx_count_ = 0;
    bool dirty_     = false;
};

} // namespace rco::renderer
