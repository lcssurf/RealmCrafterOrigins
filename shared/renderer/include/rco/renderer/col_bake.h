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

} // namespace rco::renderer
