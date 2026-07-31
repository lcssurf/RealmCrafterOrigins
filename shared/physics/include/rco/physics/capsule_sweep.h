#pragma once
#include "rco/renderer/collision_data.h"
#include <glm/glm.hpp>

// Capsule-sweep primitives — pure geometry, no state, no gameplay logic.
// Used by CharacterPhysics (character_physics.h) for both horizontal
// collide-and-slide (sweep along a movement delta) and vertical ground
// detection (sweep along a small downward delta) — the SAME functions serve
// both, since a sweep is just a delta in some direction; there's no separate
// "horizontal" vs "vertical" code path. See docs/TECH_DEBT.md #125's
// architecture investigation for why this replaces the old ColData::Resolve
// (horizontal push) + renderer::ComputeGroundHeight (vertical box/AABB
// candidate) + renderer::SampleMeshGroundHeight (vertical mesh raycast) —
// three separate systems collapsing into one.
//
// The player capsule is modelled the SAME way the rest of this codebase
// already does (see renderer::kPlayerCapsuleRadius/Height's doc comment):
// a vertical CYLINDER — flat caps gated by a [pos.y, pos.y+height] Y-band,
// not true rounded capsule end-caps. Kept consistent on purpose rather than
// silently changing the collision shape's real behavior during this
// reformulation.
namespace rco::physics {

struct SweepHit {
    bool hit = false;
    // Fraction of `delta` traveled before contact, in [0, 1]. 0 means
    // already overlapping at the start position.
    float t = 1.f;
    // World-space contact normal (points away from the surface).
    glm::vec3 normal{0.f, 1.f, 0.f};
    // Approximate world-space contact point (pos + t*delta) — good enough
    // for slide-plane projection; not a precise closest-point-on-surface.
    glm::vec3 point{0.f};
    // The obstacle's own top height, when known (box: worldYMax; triangle:
    // interpolated height at the contact XZ). Used for step-up decisions —
    // NaN-like sentinel (-FLT_MAX) when not meaningful (e.g. a sphere).
    float surface_top_y = -3.402823466e+38F;
};

// Sweeps a vertical capsule (radius R, from `pos` to `pos + (0,height,0)`)
// along `delta` against a single box, returning the first contact (if any)
// within the swept range [0,1].
//
// Technique: transforms into the box's own local frame via
// glm::transpose(box.rot) — the SAME technique already validated by the
// legacy renderer::PointInBoxFootprint/ColData::Resolve's testBox (terrain.cpp)
// — then inflates the box's XZ half-extents by R (Minkowski sum) and runs a
// standard ray-vs-slab test for the swept XZ circle's center path. This is
// EXACT along the box's flat faces; right at the corners it's a
// conservative approximation (a true capsule sweep rounds corners with a
// quarter-circle — this treats them as square), a known and accepted
// simplification (see docs/TECH_DEBT.md #125).
//
// Floor vs wall: a capsule whose FEET (pos.y) sit at or above the box's own
// top (worldYMax, within a small epsilon) never registers as a hit here,
// even if its XZ footprint overlaps — that's a "standing on it" support
// contact for CharacterPhysics::ProbeGround to own, not a horizontal wall.
// Without this, the exact object holding the player up would also block
// them from ever walking on it (see docs/TECH_DEBT.md #125's "grudado" fix).
SweepHit SweepCapsuleVsBox(const glm::vec3& pos, float radius, float height,
                            const glm::vec3& delta, const renderer::ColBox& box);

// Sweeps the same capsule against a single mesh triangle.
//
// Technique: NOT a fully analytic continuous sweep (that requires solving
// capsule-vs-inflated-triangle, i.e. the triangle's Minkowski sum with a
// capsule — edges as cylinders, vertices as spheres — meaningfully more
// math for marginal gain here). Instead, samples the path at a fixed number
// of substeps, testing circle-vs-triangle-2D overlap (the same
// ClosestPtTri2D-style closest-point technique the legacy Resolve() already
// used) at each, then bisects the first miss→hit interval a few times for a
// tighter t estimate. Safe against tunneling as long as each frame's delta
// stays well under the capsule radius — true for this game's current
// movement speeds (~8-13 u/s at 60fps ≈ 0.15-0.2 u/frame vs R=0.45); would
// need a real analytic sweep if a much faster movement ability (e.g. a
// high-speed dash) is added later. Documented, not hidden — see
// docs/TECH_DEBT.md #125.
//
// Floor vs wall: same principle as SweepCapsuleVsBox, but using the
// triangle's EXACT interpolated height at each query XZ (not its raw max
// vertex) — required for sloped/arched meshes (a curved bridge deck), where
// the tallest vertex can sit well above where the capsule's feet actually
// rest at any given point along the path.
SweepHit SweepCapsuleVsTriangle(const glm::vec3& pos, float radius, float height,
                                 const glm::vec3& delta, const renderer::ColTri& tri);

// Sweeps the same capsule against a sphere. Exact analytic 2D (XZ)
// swept-circle-vs-circle, combined radius R+sphere.radius, with the
// vertical Y-band checked only at the sweep's start/end (not swept) — an
// approximation that's fine today since no coldata.bin in this project
// currently has any spheres (confirmed 0 for Training Camp — see
// docs/TECH_DEBT.md #125's triangle-count investigation).
SweepHit SweepCapsuleVsSphere(const glm::vec3& pos, float radius, float height,
                               const glm::vec3& delta, const renderer::ColSphere& sphere);

// Real per-triangle ray-plane intersection (2D barycentric containment in
// XZ, then height interpolated from the triangle's own plane) — used by
// CharacterPhysics's ground probe for mesh surfaces. Returns true and sets
// outY when the vertical column at (x,z) passes through tri's XZ projection
// AND the resulting height is <= y_start (i.e. a ray cast straight down
// from y_start would actually reach it). Same technique as the legacy
// renderer::SampleMeshGroundHeight's RayVerticalHitsTri (terrain.cpp) —
// duplicated here rather than called, since shared/physics can't depend on
// client/ code; this is now the "real" owner of that logic.
bool RayVerticalHitsTri(float x, float z, float y_start,
                         const renderer::ColTri& tri, float& outY);

} // namespace rco::physics
