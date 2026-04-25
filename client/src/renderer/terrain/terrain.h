#pragma once
#include "renderer/terrain/terrain_chunk.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

namespace rco::renderer {

class Pipeline;

struct MatTex {
    GLuint albedo    = 0;
    GLuint normal    = 0;
    GLuint roughness = 0;
    GLuint ao        = 0;   // ambient occlusion (R channel)
    GLuint height    = 0;   // displacement height (R channel) — used for height-blend
    float  tiling    = 4.f;
};

class Terrain {
public:
    static constexpr float kCellSize  = 2.f;
    // (kSize-1) cells per chunk so adjacent chunks share their border vertex.
    static constexpr float kChunkSize = (TerrainChunk::kSize - 1) * kCellSize;

    bool  Init(int grid_w = 8, int grid_h = 8);
    bool  LoadFromEditor(const std::string& area_name);
    void  Submit(Pipeline& pipeline) const;
    void  Destroy();
    float SampleHeight(float wx, float wz) const;

private:
    void GenerateProcedural();
    void RebuildChunksFromHmap();
    void UnloadMaterials();
    GLuint MakeSolidTex(uint8_t r, uint8_t g, uint8_t b);
    GLuint LoadLinearTex(const std::string& path);
    GLuint LoadSRGBTex(const std::string& path);

    std::vector<std::unique_ptr<TerrainChunk>> chunks_;
    int grid_w_ = 0, grid_h_ = 0;

    // Heightmap loaded from editor
    std::vector<float> hmap_data_;
    int   hmap_w_        = 0, hmap_h_        = 0;
    float hmap_cell_     = 2.f;
    float hmap_origin_x_ = 0.f, hmap_origin_z_ = 0.f;
    float hmap_size_x_   = 0.f, hmap_size_z_   = 0.f;
    bool  has_hmap_      = false;

    // Splatmap GL texture
    GLuint splatmap_tex_ = 0;

    // Up to 4 materials
    std::vector<MatTex> materials_;
    GLuint def_normal_    = 0;
    GLuint def_roughness_ = 0;
    GLuint def_ao_        = 0;
    GLuint def_height_    = 0;
    bool   has_materials_ = false;
};

} // namespace rco::renderer
