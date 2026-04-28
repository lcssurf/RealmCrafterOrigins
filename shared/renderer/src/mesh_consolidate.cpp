#include "rco/renderer/mesh_consolidate.h"
#include "rco/renderer/model.h"

#include <cstdio>
#include <unordered_map>
#include <vector>
#include <string>

namespace rco::renderer {

ConsolidationResult ConsolidateMeshes(Model& model, const char* path) {
    const auto& meshes = model.meshes();

    ConsolidationResult result{};
    result.original_submesh_count     = (int)meshes.size();
    result.consolidated_submesh_count = (int)meshes.size(); // unchanged in OFF mode
    result.materials_detected         = 0;

    // Group submesh indices by material name (same logic as the original inline block).
    std::unordered_map<std::string, std::vector<int>> mat_groups;
    for (int i = 0; i < (int)meshes.size(); ++i)
        mat_groups[meshes[i].material_name].push_back(i);

    result.materials_detected = (int)mat_groups.size();

    std::fprintf(stderr,
        "[consolidate-detect] '%s' total_submeshes=%d unique_materials=%d\n",
        path ? path : "", (int)meshes.size(), (int)mat_groups.size());

    for (const auto& [mat_name, indices] : mat_groups) {
        size_t total_verts   = 0;
        size_t total_indices = 0;
        for (int si : indices) {
            total_verts   += meshes[si].raw_verts.size() / 11;
            total_indices += meshes[si].raw_indices.size();
        }
        std::fprintf(stderr,
            "[consolidate-detect]   mat='%s' submeshes=%d total_verts=%zu total_indices=%zu\n",
            mat_name.c_str(), (int)indices.size(), total_verts, total_indices);
    }

#if RCO_MESH_CONSOLIDATE == 1
    // ON mode requires SubMesh::raw_bone_ids / raw_bone_weights (step 3c).
    // Those fields do not yet exist in SubMesh — ProcessMesh discards CPU-side
    // bone data after uploading to bone_vbo. Merging bone streams without them
    // would corrupt skinning. Step 3c must:
    //   1. Add raw_bone_ids / raw_bone_weights to SubMesh in model.h.
    //   2. Populate them in ProcessMesh (alongside the existing bone_vbo upload).
    //   3. Clear them alongside raw_verts / raw_indices at the end of Load().
    // Until then, compilation with RCO_MESH_CONSOLIDATE=1 is intentionally blocked.
    static_assert(RCO_MESH_CONSOLIDATE == 0,
        "RCO_MESH_CONSOLIDATE=1 requires SubMesh::raw_bone_ids / raw_bone_weights "
        "(step 3c). Set RCO_MESH_CONSOLIDATE=0 until that step lands.");
#endif

    return result;
}

} // namespace rco::renderer
