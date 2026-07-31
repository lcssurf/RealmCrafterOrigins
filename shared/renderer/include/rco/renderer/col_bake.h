#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <array>
#include <string>

namespace rco::renderer {

// Extract all triangles from a mesh file for collision baking.
// Node transforms are baked in (aiProcess_PreTransformVertices), so output is
// in the model's root space — no additional transform applied.
// Returns false if the file cannot be opened or contains no geometry.
bool ExtractMeshTriangles(const std::string& path,
                          std::vector<std::array<glm::vec3, 3>>& out_tris);

// Result of ComputeColBoxWorldExtent: a collision box's world-space center
// and (unrotated) half-extent.
struct ColBoxWorldExtent {
    glm::vec3 center;
    glm::vec3 half;
};

// Computes a box/wedge collision shape's world-space center + half-extent
// from its LOCAL-SPACE offset/size (media_model_shapes convention — size is
// the FULL extent, not half; see the GUE Media tab's "Box/Wedge: W/H/D =
// full extents" hint) and the owning object's world pos/scale.
//
// This is THE single source of truth for this math — extracted from
// tools/gue/src/zone_scene.cpp's SaveColData/RebuildColVis (the reference
// implementation), included by BOTH the GUE (static bake + editor preview)
// and the game client (per-frame dynamic-collision recompute) so there is
// exactly one compiled definition, not two hand-copied formulas that can
// silently drift apart.
//
// rot (default identity): the object's rotation matrix. The client's
// per-frame dynamic-collision recompute (main.cpp) now passes the object's
// actual current Ry*Rx*Rz rotation here (rotating objects, e.g. spike
// traps, need this — identity broke once rotation became part of
// gameplay); identity remains just the natural default for any caller that
// truly has no rotation, not a special case: center reduces to
// `pos + offset*scale` and half is untouched.
//
// The returned half-extent is always UNROTATED (matches how ColBox itself
// stores it — center is rotated, half plus a separate rotation matrix
// describe the OBB). Callers that need to actually DRAW the rotated box
// (e.g. AppendBox's wireframe corners) apply `rot` themselves on top of
// 2.f*half (full extent) — this function does not fold rotation into the
// returned half-extent.
inline ColBoxWorldExtent ComputeColBoxWorldExtent(
    const glm::vec3& pos, const glm::vec3& scale,
    const glm::vec3& offset, const glm::vec3& size,
    const glm::mat3& rot = glm::mat3(1.f))
{
    ColBoxWorldExtent out;
    out.center = pos + rot * (offset * scale);
    out.half   = size * 0.5f * scale;
    return out;
}

} // namespace rco::renderer
