#pragma once
#include "rco/renderer/collision_data.h"
#include <glm/glm.hpp>
#include <functional>
#include <vector>

namespace rco::physics {

// A read-only VIEW that bundles every collision-data source the player-
// physics resolvers need — a single object instead of 3+ loose parameters
// (col_data, dynamic_collision_boxes, terrain) scattered across call sites.
// See docs/TECH_DEBT.md #125's architecture investigation.
//
// col_data/dynamic_boxes: now COMPLETE types (rco::renderer::ColData/ColBox,
// promoted to shared/renderer/collision_data.h — see that header's doc
// comment for why: they're pure data, no OpenGL, so they can live in
// shared/ without dragging client/ along). CharacterPhysics::Move()
// (character_physics.h/.cpp) dereferences these directly to run the capsule
// sweep against boxes and mesh triangles.
//
// sample_terrain_height/sample_terrain_normal: NOT a `const Terrain&`.
// renderer::Terrain (the heightmap/rendering class) stays client-side on
// purpose — it pulls in OpenGL/glad and belongs to the client project, not
// a shared library. These two callbacks are the ONLY two Terrain methods
// the physics module actually needs (SampleHeight/SampleNormal), passed in
// as type-erased functions so shared/physics never needs to see Terrain's
// definition at all — a standard dependency-inversion pattern, not a
// workaround. The caller (main.cpp, which already includes terrain.h)
// constructs these as thin lambdas over its own `Terrain terrain;` instance.
struct CollisionWorld {
    const rco::renderer::ColData& col_data;
    const std::vector<rco::renderer::ColBox>& dynamic_boxes;
    std::function<float(float x, float z)> sample_terrain_height;
    std::function<glm::vec3(float x, float z)> sample_terrain_normal;
};

} // namespace rco::physics
