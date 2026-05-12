#include "rco/renderer/mesh_consolidate.h"
#include "rco/renderer/model.h"

#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>
#include <utility>
#include <cstdlib>
#include <cstring>

namespace rco::renderer {

static bool VerboseConsolidationLogsEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("RCO_ASSET_LOG_VERBOSE");
        if (!v) return false;
        return std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
               std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "on") == 0 ||
               std::strcmp(v, "ON") == 0;
    }();
    return enabled;
}

ConsolidationResult ConsolidateMeshes(Model& model, const char* path) {
    const auto& meshes = model.meshes();  // const& — safe for OFF mode read

    ConsolidationResult result{};
    result.original_submesh_count     = (int)meshes.size();
    result.consolidated_submesh_count = (int)meshes.size();

    // Group submesh indices by material name (preserves order via first-seen index).
    std::unordered_map<std::string, std::vector<int>> mat_groups;
    for (int i = 0; i < (int)meshes.size(); ++i)
        mat_groups[meshes[i].material_name].push_back(i);

    result.materials_detected = (int)mat_groups.size();

    // ── Detection log (always emitted, OFF and ON mode) ───────────────────────
    if (VerboseConsolidationLogsEnabled()) {
        std::fprintf(stderr,
            "[consolidate-detect] '%s' total_submeshes=%d unique_materials=%d\n",
            path ? path : "", (int)meshes.size(), (int)mat_groups.size());
    }

    for (const auto& [mat_name, indices] : mat_groups) {
        size_t total_verts   = 0;
        size_t total_indices = 0;
        for (int si : indices) {
            total_verts   += (size_t)meshes[si].raw_vertex_count;
            total_indices += meshes[si].raw_indices.size();
        }
        if (VerboseConsolidationLogsEnabled()) {
            std::fprintf(stderr,
                "[consolidate-detect]   mat='%s' submeshes=%d total_verts=%zu total_indices=%zu\n",
                mat_name.c_str(), (int)indices.size(), total_verts, total_indices);
        }
    }

#if RCO_MESH_CONSOLIDATE == 1
    // ── ON mode: merge vertex/index/bone buffers per material group ───────────
    //
    // Invariants assumed:
    //   • All submeshes in a material group share the same bone skeleton.
    //     bone_offsets from the FIRST submesh in the group are inherited.
    //     (Skinned models exported from one object in DCC tools satisfy this.)
    //   • Submeshes may be skinned or static; mixed groups produce a skinned
    //     consolidated mesh (static slots get zero bone data = bone-0 weight-0,
    //     which the vertex shader maps to identity — harmless for static parts).

    // Build processing order sorted by first submesh index to preserve draw order.
    std::vector<std::pair<int, std::string>> ordered;
    ordered.reserve(mat_groups.size());
    for (const auto& [name, idxs] : mat_groups)
        ordered.emplace_back(idxs.front(), name);
    std::sort(ordered.begin(), ordered.end());

    bool any_merged = false;
    std::vector<SubMesh> new_meshes;
    new_meshes.reserve(meshes.size());

    for (const auto& [/*first_idx*/_, mat_name] : ordered) {
        const auto& idxs = mat_groups[mat_name];

        if (idxs.size() == 1) {
            // Single submesh — carry through unchanged.
            new_meshes.push_back(std::move(model.meshes_[idxs[0]]));
            continue;
        }

        // ── Merge ────────────────────────────────────────────────────────────
        std::vector<float>    merged_verts;
        std::vector<unsigned> merged_indices;
        std::vector<int>      merged_bone_ids;
        std::vector<float>    merged_bone_wts;
        unsigned              vertex_offset = 0;
        bool                  any_skinned   = false;
        const glm::mat4       kIdentity(1.f);

        const SubMesh& first_sm = model.meshes_[idxs[0]];

        for (int si : idxs) {
            const SubMesh& src = model.meshes_[si];
            if (src.skinned) any_skinned = true;

            // ── Vertex-space correction ───────────────────────────────────────
            // If this submesh has different bone-offset matrices than first_sm,
            // its vertices are in a different mesh-local space. Compute the
            // rigid correction that maps src's space → first_sm's space:
            //   correction = inverse(first.offset[b]) * src.offset[b]
            //              = node_first * inv(node_src)   (same for all bones b)
            // Apply to pos/normal/tangent before concatenating.
            glm::mat4 corr(1.f);
            glm::mat3 corr_n(1.f);
            bool apply_corr = false;
            if (si != idxs[0]) {
                for (int b = 0; b < (int)first_sm.bone_offsets.size() &&
                                b < (int)src.bone_offsets.size(); ++b) {
                    if (first_sm.bone_offsets[b] == kIdentity ||
                        src.bone_offsets[b]       == kIdentity) continue;
                    glm::mat4 c = glm::inverse(first_sm.bone_offsets[b]) * src.bone_offsets[b];
                    float d = 0.f;
                    for (int col = 0; col < 4; ++col)
                        for (int row = 0; row < 4; ++row)
                            d += std::abs(c[col][row] - kIdentity[col][row]);
                    if (d > 0.001f) {
                        corr   = c;
                        corr_n = glm::mat3(glm::transpose(glm::inverse(corr)));
                        apply_corr = true;
                    }
                    break; // one pivot bone is enough — correction is space-uniform
                }
            }

            // Geometry — 11 floats/vertex: pos3|norm3|uv2|tan3
            if (!apply_corr) {
                merged_verts.insert(merged_verts.end(),
                                    src.raw_verts.begin(), src.raw_verts.end());
            } else {
                const int nv = src.raw_vertex_count;
                merged_verts.reserve(merged_verts.size() + (size_t)nv * 11);
                for (int vi = 0; vi < nv; ++vi) {
                    const float* v = &src.raw_verts[(size_t)vi * 11];
                    glm::vec3 pos(v[0], v[1], v[2]);
                    glm::vec3 nor(v[3], v[4], v[5]);
                    glm::vec3 tan(v[8], v[9], v[10]);
                    pos = glm::vec3(corr * glm::vec4(pos, 1.f));
                    nor = glm::normalize(corr_n * nor);
                    tan = glm::normalize(corr_n * tan);
                    merged_verts.push_back(pos.x); merged_verts.push_back(pos.y); merged_verts.push_back(pos.z);
                    merged_verts.push_back(nor.x); merged_verts.push_back(nor.y); merged_verts.push_back(nor.z);
                    merged_verts.push_back(v[6]);  merged_verts.push_back(v[7]);
                    merged_verts.push_back(tan.x); merged_verts.push_back(tan.y); merged_verts.push_back(tan.z);
                }
            }

            // Indices with accumulated vertex offset.
            for (unsigned idx : src.raw_indices)
                merged_indices.push_back(idx + vertex_offset);
            vertex_offset += (unsigned)src.raw_vertex_count;

            // Bone data — 4 ints + 4 floats per vertex (zeros for non-skinned).
            // Bone IDs are global indices — unchanged by space correction.
            merged_bone_ids.insert(merged_bone_ids.end(),
                                   src.raw_bone_ids.begin(),     src.raw_bone_ids.end());
            merged_bone_wts.insert(merged_bone_wts.end(),
                                   src.raw_bone_weights.begin(), src.raw_bone_weights.end());
        }

        // Build consolidated SubMesh — copy material metadata from first submesh.
        const SubMesh& first = model.meshes_[idxs[0]];
        SubMesh sm;
        sm.material_name    = first.material_name;
        sm.material_idx     = first.material_idx;
        sm.tex_albedo       = first.tex_albedo;
        sm.tex_normal       = first.tex_normal;
        sm.tex_orm          = first.tex_orm;
        sm.orm_packed       = first.orm_packed;
        sm.albedo_factor    = first.albedo_factor;
        sm.roughness_factor = first.roughness_factor;
        sm.metallic_factor  = first.metallic_factor;
        sm.skinned          = any_skinned;

        // Merge bone_offsets across all submeshes in the group.
        // Each submesh has offsets only for the bones that affect its own vertices;
        // slots for unreferenced bones are padded with identity by Load().
        // Merging: for each bone slot, take the first non-identity value found.
        // For well-formed exports (shared skeleton, same mesh-local space) offsets
        // for a given bone are identical across all submeshes that reference it,
        // so the source submesh chosen per slot doesn't matter — correctness holds.
        sm.bone_offsets = first.bone_offsets;
        for (int si : idxs) {
            const auto& src = model.meshes_[si].bone_offsets;
            for (int b = 0; b < (int)src.size(); ++b) {
                if (b >= (int)sm.bone_offsets.size())
                    sm.bone_offsets.resize(b + 1, kIdentity);
                if (sm.bone_offsets[b] == kIdentity && src[b] != kIdentity)
                    sm.bone_offsets[b] = src[b];
            }
        }


        sm.idx_count        = (int)merged_indices.size();
        sm.raw_vertex_count = (int)vertex_offset;

        // ── GL objects — identical layout to ProcessMesh ──────────────────────
        GLint prev_vao = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);

        glGenVertexArrays(1, &sm.vao);
        glGenBuffers(1, &sm.vbo);
        glGenBuffers(1, &sm.ebo);
        glBindVertexArray(sm.vao);

        glBindBuffer(GL_ARRAY_BUFFER, sm.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(merged_verts.size() * sizeof(float)),
                     merged_verts.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sm.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(merged_indices.size() * sizeof(unsigned)),
                     merged_indices.data(), GL_STATIC_DRAW);

        // Attrib layout matches terrainGBuffer.vs / actor shaders:
        // loc 0 = vec3 pos   (offset  0), loc 1 = vec3 normal (offset 12),
        // loc 2 = vec2 uv    (offset 24), loc 3 = vec3 tangent (offset 32)
        constexpr int stride = 11 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);   glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)12);  glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)24);  glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void*)32);  glEnableVertexAttribArray(3);

        if (any_skinned && !merged_bone_ids.empty()) {
            // Interleaved bone buffer: [ivec4 ids (16 B) | vec4 weights (16 B)]
            // Matches the BoneVertex layout in ProcessMesh.
            struct BoneVtx { glm::ivec4 ids; glm::vec4 wts; };
            std::vector<BoneVtx> bv(vertex_offset);
            for (unsigned i = 0; i < vertex_offset; ++i) {
                bv[i].ids = glm::ivec4(merged_bone_ids[i*4+0], merged_bone_ids[i*4+1],
                                       merged_bone_ids[i*4+2], merged_bone_ids[i*4+3]);
                bv[i].wts = glm::vec4(merged_bone_wts[i*4+0], merged_bone_wts[i*4+1],
                                       merged_bone_wts[i*4+2], merged_bone_wts[i*4+3]);
            }
            glGenBuffers(1, &sm.bone_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, sm.bone_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(bv.size() * sizeof(BoneVtx)),
                         bv.data(), GL_STATIC_DRAW);

            constexpr int bstride = (int)sizeof(BoneVtx);
            glVertexAttribIPointer(4, 4, GL_INT,   bstride, (void*)0);
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(5,  4, GL_FLOAT, GL_FALSE, bstride,
                                  (void*)sizeof(glm::ivec4));
            glEnableVertexAttribArray(5);
        }

        glBindVertexArray((GLuint)prev_vao);

        // Null out GL handles on old submeshes before discarding them so the
        // Model destructor's FreeMesh doesn't double-delete.
        for (int si : idxs) {
            SubMesh& old = model.meshes_[si];
            if (old.vao)      { glDeleteVertexArrays(1, &old.vao);  old.vao      = 0; }
            if (old.vbo)      { glDeleteBuffers(1, &old.vbo);       old.vbo      = 0; }
            if (old.ebo)      { glDeleteBuffers(1, &old.ebo);       old.ebo      = 0; }
            if (old.bone_vbo) { glDeleteBuffers(1, &old.bone_vbo);  old.bone_vbo = 0; }
        }

        // Keep merged raw data alive until Load()'s cleanup pass (consistent
        // with how ProcessMesh leaves raw_verts/raw_indices on single submeshes).
        sm.raw_verts        = std::move(merged_verts);
        sm.raw_indices      = std::move(merged_indices);
        sm.raw_bone_ids     = std::move(merged_bone_ids);
        sm.raw_bone_weights = std::move(merged_bone_wts);

        if (VerboseConsolidationLogsEnabled()) {
            std::fprintf(stderr,
                "[mesh-consolidate]   mat='%s' merged %d submeshes -> 1 (%u verts, %d indices)\n",
                mat_name.c_str(), (int)idxs.size(), vertex_offset, sm.idx_count);
        }

        new_meshes.push_back(std::move(sm));
        any_merged = true;
    }

    if (any_merged) {
        model.meshes_ = std::move(new_meshes);
        result.consolidated_submesh_count = (int)model.meshes_.size();
        if (VerboseConsolidationLogsEnabled()) {
            std::fprintf(stderr,
                "[mesh-consolidate] '%s' consolidated %d -> %d submeshes\n",
                path ? path : "",
                result.original_submesh_count,
                result.consolidated_submesh_count);
        }
    }
#endif // RCO_MESH_CONSOLIDATE == 1

    return result;
}

} // namespace rco::renderer
