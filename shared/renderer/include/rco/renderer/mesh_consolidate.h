#pragma once

#ifndef RCO_MESH_CONSOLIDATE
#define RCO_MESH_CONSOLIDATE 0  // OFF by default — set to 1 only after step 3c
#endif

namespace rco { namespace renderer {

class Model;

struct ConsolidationResult {
    int original_submesh_count     = 0;
    int consolidated_submesh_count = 0;
    int materials_detected         = 0;
};

// Analyses submeshes by material name and logs grouping info.
// RCO_MESH_CONSOLIDATE == 0 (default): detection + log only, no GPU changes.
// RCO_MESH_CONSOLIDATE == 1: merges vertex/index buffers per material group.
//   Requires SubMesh::raw_bone_ids / raw_bone_weights (step 3c) — not yet present.
//   Attempting to compile with 1 triggers a static_assert to prevent silent breakage.
ConsolidationResult ConsolidateMeshes(Model& model, const char* path = "");

}} // namespace rco::renderer
