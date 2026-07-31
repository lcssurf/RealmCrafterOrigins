#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>

// Pure collision-data types (no OpenGL/rendering dependency) — promoted out
// of client/src/renderer/terrain/terrain.h during the player-physics
// reformulation (see docs/TECH_DEBT.md #125's architecture investigation)
// so shared/physics (which must not depend on the client project) can
// reference ColBox/ColSphere/ColTri/ColData directly. Terrain itself (the
// heightmap/rendering class) stays client-side — it genuinely needs GL.
//
// ColData::Resolve()/LoadColData() are legacy as of the physics
// reformulation: the player no longer calls them (see
// shared/physics/character_physics.h's unified capsule sweep, which
// reimplements horizontal push + ground detection against these same data
// types). Left here, still fully functional, in case anything else
// (future NPC client-side collision, tooling) ends up needing the same
// "resolve a circle against boxes/spheres/tris" behavior — not deleted
// speculatively.
namespace rco::renderer {

struct ColBox {
    glm::vec3 pos;   // center
    glm::vec3 half;  // half-extents (scale / 2), in the box's OWN local axes
    // Local-to-world rotation (identity = axis-aligned, the only case COLD v2
    // files produce). COLD v3 adds pitch/yaw/roll per box (same Ry*Rx*Rz
    // convention used everywhere else — see zone_scene.cpp's `trs`), letting
    // a rotated wall/door stay a true oriented box instead of an
    // ever-growing re-fit AABB (which was correct only at 0/90/180/270deg
    // and became an oversized box at any other angle).
    glm::mat3 rot = glm::mat3(1.f);
    // World-space Y bounds of the rotated box (precomputed at load), used
    // only for cheap early-outs (both the legacy Resolve() and the new
    // capsule-sweep ground probe) — NOT the collision shape itself.
    float worldYMin = 0.f, worldYMax = 0.f;
};

struct ColSphere {
    glm::vec3 pos;
    float     radius;
};

struct ColTri {
    glm::vec3 v[3];  // world-space triangle vertices
};

// Player collision capsule dimensions. Exposed here (not a local constexpr)
// so any code that needs to draw/reason about the EXACT capsule the
// collision system uses — the client's F11 debug overlay, the new
// shared/physics CharacterPhysics::Config defaults — reads the same single
// source of truth instead of hand-copying the numbers.
constexpr float kPlayerCapsuleRadius = 0.45f;
constexpr float kPlayerCapsuleHeight = 1.8f;

// Max obstacle-top height, measured from the player's current feet, that's
// treated as a climbable step instead of a solid wall. 0.5 units against a
// 1.8-unit-tall capsule is a curb/stair-riser proportion (~28% of height,
// generous by real-world standards but a common games feel for "obviously
// steppable ledge").
constexpr float kMaxStepHeight = 0.5f;

struct ColData {
    std::vector<ColBox>    boxes;
    std::vector<ColSphere> spheres;
    std::vector<ColTri>    tris;    // mesh collision triangles (v2+)
    bool loaded = false;

    // LEGACY (see file doc comment above) — resolves player XZ position
    // against all volumes as a vertical capsule: radius R, height H, feet at
    // (px, py, pz). Only XZ is modified; py stays owned by the caller.
    // dynamicBoxes (optional): world-space AABBs rebuilt every frame by the
    // caller from is_dynamic scenery objects' current pose — tested inside
    // the SAME 3-iteration convergence loop as boxes/spheres/tris below.
    void Resolve(float& px, float pz_in, float py, float& out_pz,
                 const std::vector<ColBox>& dynamicBoxes = {}) const;
};

// Loads coldata.bin for the given area name. Returns an empty ColData on failure.
ColData LoadColData(const std::string& area_name);

} // namespace rco::renderer
