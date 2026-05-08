#pragma once
#include "renderer/terrain/terrain_chunk.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

namespace rco::renderer {

struct ColBox {
    glm::vec3 pos;   // center
    glm::vec3 half;  // half-extents (scale / 2)
};

struct ColSphere {
    glm::vec3 pos;
    float     radius;
};

struct ColTri {
    glm::vec3 v[3];  // world-space triangle vertices
};

struct ColData {
    std::vector<ColBox>    boxes;
    std::vector<ColSphere> spheres;
    std::vector<ColTri>    tris;    // mesh collision triangles (v2+)
    bool loaded = false;

    // Resolve player XZ position against all volumes. Player is modelled as a
    // vertical capsule: radius R, height H, feet at (px, py, pz).
    // Only XZ is modified; py (terrain height) stays owned by the terrain system.
    void Resolve(float& px, float pz_in, float py, float& out_pz) const;
};

// Loads coldata.bin for the given area name. Returns an empty ColData on failure.
ColData LoadColData(const std::string& area_name);

class Pipeline;

struct MatTex {
    GLuint albedo          = 0;
    GLuint normal          = 0;
    GLuint roughness       = 0;
    GLuint ao              = 0;    // ambient occlusion (R channel)
    GLuint height          = 0;    // displacement height (R channel) — used for height-blend
    float  tiling          = 4.f;
    float  normal_strength = 2.5f; // per-material normal map intensity
};

class Terrain {
public:
    static constexpr float kCellSize  = 2.f;
    // (kSize-1) cells per chunk so adjacent chunks share their border vertex.
    static constexpr float kChunkSize = (TerrainChunk::kSize - 1) * kCellSize;

    bool      Init(int grid_w = 8, int grid_h = 8);
    bool      LoadFromEditor(const std::string& area_name);
    void      Submit(Pipeline& pipeline, const glm::vec3& cam_pos = glm::vec3(0.f)) const;
    void      Destroy();
    float     SampleHeight(float wx, float wz) const;
    glm::vec3 SampleNormal(float wx, float wz) const;
    float     SlopeAngle(float wx, float wz) const;

private:
    void GenerateProcedural();
    void RebuildChunksFromHmap();
    void UnloadMaterials();
    GLuint MakeSolidTex(uint8_t r, uint8_t g, uint8_t b);
    GLuint LoadLinearTex(const std::string& path);
    GLuint LoadSRGBTex(const std::string& path);

    std::vector<std::unique_ptr<TerrainChunk>> chunks_;
    int grid_w_ = 0, grid_h_ = 0;

    // Heightmap data (CPU) and GPU texture (R32F)
    std::vector<float> hmap_data_;
    int   hmap_w_        = 0, hmap_h_        = 0;
    float hmap_cell_     = 2.f;
    float hmap_origin_x_ = 0.f, hmap_origin_z_ = 0.f;
    float hmap_size_x_   = 0.f, hmap_size_z_   = 0.f;
    bool  has_hmap_      = false;
    GLuint hmap_tex_     = 0;   // R32F, uploaded from hmap_data_

    // Splatmap GL texture
    GLuint splatmap_tex_  = 0;

    // Macro variation — grayscale texture, covers full terrain, breaks tiling repetition
    GLuint macro_tex_      = 0;
    float  macro_strength_ = 0.0f;
    GLuint def_macro_      = 0;  // fallback 1×1 neutral gray (no effect)

    // Up to 4 materials
    std::vector<MatTex> materials_;
    GLuint def_normal_    = 0;
    GLuint def_roughness_ = 0;
    GLuint def_ao_        = 0;
    GLuint def_height_    = 0;
    bool   has_materials_ = false;
};

} // namespace rco::renderer
