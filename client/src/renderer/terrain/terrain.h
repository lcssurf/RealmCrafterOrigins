#pragma once
#include "renderer/terrain/terrain_chunk.h"
#include "rco/renderer/material_texture_array.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

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

struct TerrainRenderTuning {
    float tiling_mul         = 1.00f; // multiplies per-layer tiling from materials.txt
    float macro_strength_mul = 1.00f; // multiplies macro strength loaded from macro.png
    float height_blend_slop  = 0.20f; // larger = smoother layer transitions
};

class Terrain {
public:
    static constexpr float kCellSize  = 2.f;
    // (kSize-1) cells per chunk so adjacent chunks share their border vertex.
    static constexpr float kChunkSize = (TerrainChunk::kSize - 1) * kCellSize;

    bool      Init(int grid_w = 8, int grid_h = 8);
    bool      LoadFromEditor(const std::string& area_name);
    void      SetRenderTuning(const TerrainRenderTuning& tuning);
    const TerrainRenderTuning& RenderTuning() const { return render_tuning_; }
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

    // Materials — no longer capped at 4 (Phase 1: >4 uses the generalized
    // N-material shader path via the texture arrays below; <=4 keeps the
    // legacy exact-4-slot path unchanged). See docs/TECH_DEBT.md "Terrain
    // multi-material authoring (Phase 1)".
    std::vector<MatTex> materials_;
    GLuint def_normal_    = 0;
    GLuint def_roughness_ = 0;
    GLuint def_ao_        = 0;
    GLuint def_height_    = 0;
    bool   has_materials_ = false;

    // N-material path (only populated/used when materials_.size() > 4) —
    // one array layer per material, built from the same already-loaded 2D
    // GL textures in materials_[i] via MaterialTextureArray::SetLayerFromGLTexture.
    rco::renderer::MaterialTextureArray mat_albedo_array_;
    rco::renderer::MaterialTextureArray mat_normal_array_;
    rco::renderer::MaterialTextureArray mat_roughness_array_;
    rco::renderer::MaterialTextureArray mat_ao_array_;
    rco::renderer::MaterialTextureArray mat_height_array_;
    void RebuildMaterialArrays();

    // Splatmap weight layers — CPU-side copies of every RSPN layer (kept
    // around so RebuildMaterialArrays-equivalent rebuilds aren't needed;
    // the splatmap itself never changes at runtime on the client) plus the
    // GPU array built from them for the N-material path.
    int splatmap_w_ = 0, splatmap_h_ = 0;
    std::vector<std::vector<uint8_t>> splatmap_layers_;
    rco::renderer::SplatWeightArray splatmap_array_;

    TerrainRenderTuning render_tuning_{};
};

} // namespace rco::renderer
