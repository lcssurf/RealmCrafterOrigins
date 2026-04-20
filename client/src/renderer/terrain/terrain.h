#pragma once
#include "renderer/terrain/terrain_chunk.h"
#include "renderer/shader.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>

namespace rco::renderer {

class Terrain {
public:
    static constexpr float kCellSize  = 2.f;
    static constexpr float kChunkSize = TerrainChunk::kSize * kCellSize;

    bool  Init(const char* shader_dir, int grid_w = 8, int grid_h = 8);
    void  Render(const glm::mat4& view, const glm::mat4& proj,
                 const glm::vec3& cam_pos, const glm::vec3& sun_dir);
    void  Destroy();
    float SampleHeight(float wx, float wz) const;

private:
    void GenerateProcedural();

    Shader shader_;
    std::vector<std::unique_ptr<TerrainChunk>> chunks_;
    int grid_w_ = 0, grid_h_ = 0;
};

} // namespace rco::renderer
