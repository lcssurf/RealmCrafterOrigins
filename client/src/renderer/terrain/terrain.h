#pragma once
#include "renderer/terrain/terrain_chunk.h"
#include "rco/renderer/material_texture_array.h"
#include "rco/renderer/collision_data.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

// ColBox/ColSphere/ColTri/ColData/kPlayerCapsuleRadius/kPlayerCapsuleHeight/
// kMaxStepHeight/LoadColData used to be declared right here — moved to
// shared/renderer/include/rco/renderer/collision_data.h (both in
// rco::renderer, so nothing below needed to change) during the player-
// physics reformulation, so shared/physics could reference them without
// depending on client/. See docs/TECH_DEBT.md #125.

namespace rco::renderer {

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

// LEGACY as of the player-physics reformulation (docs/TECH_DEBT.md #125):
// the player no longer calls this — PlayerController now resolves ground
// detection through rco::physics::CharacterPhysics::Move()'s unified
// capsule sweep (shared/physics), which reimplements the same box-footprint
// and mesh-raycast candidates directly against CollisionWorld. Left here,
// still fully functional, since nothing forces removal (may still be useful
// for camera/tooling/future NPC collision).
//
// Highest walkable surface at (x, z): the terrain heightmap, the top
// (worldYMax) of a static/dynamic Box collision volume whose XZ footprint
// contains the point, OR the height of a col_data.tris mesh triangle
// directly below (x, z) — whichever is highest. Lets the player stand on
// elevated objects (bridges, platforms, arches — ANY shape, not just
// flat-topped ones) instead of always falling back to the sculpted terrain
// underneath them.
//
// Mesh candidate is a real vertical raycast (see SampleMeshGroundHeight in
// terrain.cpp) against the SAME triangles ColData::Resolve() already tests
// for horizontal push — not an AABB/bounding-box approximation. This
// replaces an earlier model-AABB-based approximation (TECH_DEBT.md #125)
// that broke down for any mesh with structure above the walkable surface
// (a bridge's railings) or non-flat geometry (an arched deck, where the
// walkable height changes continuously — no single fixed height/box could
// ever represent it correctly). Confirmed as the actual industry pattern
// this round: both RealmCrafter-Standard's Blitz3D collision
// (CollisionNY# contact-normal check against real terrain+mesh geometry)
// and Unreal's CharacterMovementComponent::FindFloor (capsule/line sweep
// via SweepSingleByChannel against real physics collision) do a real
// downward query against actual geometry, never a per-object bounding-box
// stand-in.
//
// support_ceiling_y gates which candidates are even considered: only
// surfaces at or below this Y count as ground (mesh: only tri hits with
// height <= support_ceiling_y; box: only worldYMax <= support_ceiling_y).
// This is what stops a tall crate/pillar/railing from yanking the player up
// onto its roof just because its footprint happens to overlap their feet —
// callers pass the player's pre-step Y (plus a small step-up tolerance
// where appropriate) so only things at-or-below roughly where the player
// already is can act as ground, never something above their head.
//
// Box footprint test assumes yaw-only rotation (ColBox.rot's only case in
// practice today, per its own doc comment) — a probe point at the box's own
// center height is transformed into the box's local frame, so pitch/roll
// would make this an approximation, not exact.
//
// dynamicBoxes: real collision volumes (is_dynamic Box/Wedge shapes) that
// ALSO get tested by ColData::Resolve()'s horizontal push — pass the exact
// same list given to Resolve() so ground and push agree on what's solid.
float ComputeGroundHeight(float x, float z, float support_ceiling_y,
                           const Terrain& terrain, const ColData& col_data,
                           const std::vector<ColBox>& dynamicBoxes = {});

// Vertical raycast from y_start downward against tris: returns the highest
// triangle-surface height at (x, z) that is <= y_start (i.e. the first
// surface a real ray cast straight down from y_start would hit — if
// multiple triangles are hit at the same XZ column, e.g. a bridge deck AND
// a pillar underneath it, the higher one wins, matching what an actual
// downward ray would report), or std::nullopt if no triangle's XZ
// projection contains the point at any height <= y_start.
//
// This is a real per-triangle ray-plane intersection (barycentric
// containment test in XZ, then height interpolated from the triangle's own
// plane) — not an approximation of the triangle soup by any bounding shape,
// so it's exact for arbitrary geometry (arches, ramps, curved decks).
std::optional<float> SampleMeshGroundHeight(float x, float z, float y_start,
                                             const std::vector<ColTri>& tris);

} // namespace rco::renderer
