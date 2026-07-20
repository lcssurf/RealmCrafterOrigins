#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>

struct aiNode;
struct aiScene;
struct aiMesh;

namespace rco::renderer {

static constexpr int kMaxBones = 64;

// ---------------------------------------------------------------------------
// Geometry submesh (one draw call)
// ---------------------------------------------------------------------------
struct SubMesh {
    GLuint vao      = 0;
    GLuint vbo      = 0;  // pos/normal/uv/tangent
    GLuint bone_vbo = 0;  // ivec4 bone_ids + vec4 bone_weights; 0 if not skinned
    GLuint ebo      = 0;
    int    idx_count = 0;
    bool   skinned   = false;

    GLuint tex_albedo   = 0;
    GLuint tex_normal   = 0;
    GLuint tex_orm      = 0;
    GLuint tex_opacity  = 0;
    GLuint tex_ao       = 0;
    bool   orm_packed   = true;

    glm::vec3 albedo_factor    = {0.72f, 0.68f, 0.60f};
    float     roughness_factor = 0.5f;
    float     metallic_factor  = 0.0f;

    // Index into Engine::materials() bindless SSBO. Filled by Load() when a
    // MaterialManager is passed; otherwise stays 0 and the submesh shares the
    // default material slot.
    int       material_idx = 0;

    // Name of the aiMaterial this submesh was exported with — used by
    // Model::ApplyMaterialsByName to match against user-imported textures
    // (Substance Painter's "ID01"/"ID02"/… typically come through intact).
    std::string material_name;

    // Per-submesh bone inverse-bind matrices. In multi-part skinned models
    // each aiMesh brings its OWN mOffsetMatrix per bone because every part
    // sits in a different mesh-local space. If two meshes share a bone name
    // but had different local origins, their offsets differ — storing only
    // one globally would render 43 of 44 parts in wrong positions.
    // Sized to bones_.size() after Load finishes; unused slots are identity.
    std::vector<glm::mat4> bone_offsets;

    // Temporary CPU copies kept alive until the end of Model::Load() so that
    // consolidation can inspect and merge raw geometry. Cleared before Load returns.
    std::vector<float>    raw_verts;        // 11 floats per vertex (pos3|norm3|uv2|tan3)
    std::vector<unsigned> raw_indices;
    std::vector<int>      raw_bone_ids;     // 4 per vertex (global bone indices)
    std::vector<float>    raw_bone_weights; // 4 per vertex (normalised)
    int                   raw_vertex_count = 0;
};

// ---------------------------------------------------------------------------
// Animation structures
// ---------------------------------------------------------------------------
struct AnimKey3 { double t; glm::vec3 v; };
struct AnimKeyQ { double t; glm::quat v; };

struct BoneChannel {
    std::string            name; // node this channel drives
    std::vector<AnimKey3>  pos;
    std::vector<AnimKeyQ>  rot;
    std::vector<AnimKey3>  scl;
};

struct AnimClip {
    std::string               name;
    float                     duration_sec = 0.f;
    float                     fps          = 30.f;  // ticks-per-second from Assimp; default 30
    std::vector<BoneChannel>  channels;
    std::map<std::string,int> chan_map; // node name → channels[] index
    // Model::bone_alias_revision_ at the moment this clip's channels were
    // resolved/retargeted (see AppendAnimationsFrom). If SetBoneAliases or
    // SetBoneRetargetOffsets is called AFTER this clip was appended, this
    // stamp goes stale and the clip's baked rotations no longer reflect the
    // current alias/offset maps — AppendAnimationsFrom's name-match
    // fast-path checks this before trusting an already-appended clip.
    int                        built_with_alias_revision = -1;
};

// Node in the bone hierarchy stored in topological order (parent before child).
struct AnimNode {
    std::string name;
    int         parent   = -1;
    glm::mat4   local    = glm::mat4(1.f); // bind-pose local transform
    int         bone_idx = -1;             // index into bones_[] or -1
};

struct BoneInfo {
    glm::mat4 offset = glm::mat4(1.f); // inverse bind-pose matrix
};

// ---------------------------------------------------------------------------
// Model — skinned PBR model loader. Geometry is submitted to Pipeline via
// the SubMesh list; this class does not render itself.
// ---------------------------------------------------------------------------

class MaterialManager;

// Forward declarations for ConsolidateMeshes friend access.
struct ConsolidationResult;
ConsolidationResult ConsolidateMeshes(class Model& model, const char* path);

// Sentinel "path" recognized by ModelCacheGet() to build a procedural UV
// sphere instead of loading a file — used by tool previews (e.g. the GUE
// Materials tab) that need to show a PBR material applied to real geometry
// without shipping a dedicated sphere asset.
inline constexpr const char* kSpherePrimitivePath = "__primitive_sphere__";

class Model {
public:
    ~Model();

    bool log_bones = false;  // enable verbose load/animation diagnostics

    // Load geometry + textures from `path`. If `mm` is provided, each submesh's
    // textures are also registered in the MaterialManager and `material_idx`
    // is written back so the submesh can be drawn via the deferred pipeline
    // without a hardcoded material slot.
    bool Load(const char* path, MaterialManager* mm = nullptr);

    // Builds a procedural UV sphere (unit-ish, radius in world units) in place
    // of loading a file — used for the material-preview sphere in the GUE.
    // Vertex layout matches ProcessMesh's (pos3|norm3|uv2|tan3), so the result
    // works with OverrideMaterial/ApplyMaterialsByName like any loaded model.
    void GenerateSpherePrimitive(float radius = 0.5f, int rings = 24, int slices = 32);

    void Destroy();
    bool IsLoaded()      const { return !meshes_.empty(); }
    const std::vector<SubMesh>& meshes() const { return meshes_; }

    // Y-extent of the model in bind-pose model space (feet assumed at y≈0).
    // Multiply by Actor::scale to get the world-space height.
    float MaxY() const { return aabb_max_y_; }

    // Returns the post-flip UV transform that was actually baked into this
    // model's vertex data — either from the .uv sidecar or from the
    // Assimp-detected KHR_texture_transform. The GUE uses this to
    // consolidate live slider deltas with the existing transform when
    // persisting a new sidecar.
    glm::vec2 EffectiveUVOffset() const { return uv_effective_offset_; }
    glm::vec2 EffectiveUVScale()  const { return uv_effective_scale_;  }

    // Full axis-aligned bounding box in bind-pose model space.
    glm::vec3 BoundsMin() const { return aabb_min_; }
    glm::vec3 BoundsMax() const { return aabb_max_; }

    // Unique aiMaterial names referenced by this model's submeshes, in the
    // order they first appear. Useful for building UI that lets the user
    // map each file-internal material to a user-created media_material.
    std::vector<std::string> MaterialNames() const;

    // Diagnostic: called by Actor::Submit once per draw submission. Returns true
    // (and decrements the counter) for the next N submits after OverrideMaterial
    // ran, so the draw logs are emitted without spamming every frame.
    bool TakeSubmitDiagLog() const {
        if (diag_log_submits_remaining_ <= 0) return false;
        --diag_log_submits_remaining_;
        return true;
    }

    bool  HasAnimations()       const { return !clips_.empty(); }
    int   ClipCount()           const { return (int)clips_.size(); }
    const std::string& ClipName(int i)     const { return clips_[i].name; }
    float ClipDuration(int i)   const {
        return (i >= 0 && i < (int)clips_.size()) ? clips_[i].duration_sec : 0.f;
    }
    float ClipFps(int i)        const {
        return (i >= 0 && i < (int)clips_.size()) ? clips_[i].fps : 30.f;
    }

    // Returns the names of all bones in this model (sorted alphabetically by
    // map iteration). Empty if the model has no skeletal data. Used by the GUE
    // socket editor to populate the bone dropdown without querying the DB.
    std::vector<std::string> BoneNames() const {
        std::vector<std::string> out;
        out.reserve(bone_map_.size());
        for (const auto& [name, _] : bone_map_) out.push_back(name);
        return out;
    }

    // Returns the names of ALL nodes in this model's hierarchy (sorted
    // alphabetically by map iteration) — unlike BoneNames(), NOT restricted
    // to nodes that actually deform a mesh vertex (bone_map_ only gets
    // entries from aiMesh::mBones; pure-transform helper/socket nodes never
    // appear there). This is the same universe AppendAnimationsFrom's
    // retargeting fallback resolves against (node_map_, populated
    // unconditionally for every aiNode in BuildNodeTree) — so any node here
    // is already a valid Bone Slot target, even ones BoneNames() hides.
    std::vector<std::string> AllNodeNames() const {
        std::vector<std::string> out;
        out.reserve(node_map_.size());
        for (const auto& [name, _] : node_map_) out.push_back(name);
        return out;
    }

    // Standalone helper for editor UIs (GUE's Bone Slots combo): opens `path`
    // in a SEPARATE, throwaway Assimp::Importer::ReadFile call — without
    // aiProcess_OptimizeGraph — purely to list every node name in the raw
    // hierarchy, then discards the scene. aiProcess_OptimizeGraph is an
    // established invariant of the real Model::Load path (skinning/pipeline
    // depends on the transform-collapsed graph it produces) and must not be
    // removed there just to recover leaf nodes it merges away (e.g.
    // Male_01.b3d's L_Shin/R_Shin, children of Bip01 L/R Calf). This function
    // never touches node_map_/bone_map_/anim_nodes_ on any live Model — its
    // result is only ever used to populate a UI dropdown.
    static std::vector<std::string> AllNodeNamesUnoptimized(const char* path);

    // Bind-pose (no animation) GLOBAL transform of a node in THIS model's own
    // hierarchy — composed by walking node_map_/anim_nodes_ from the root
    // down to `name` and multiplying each ancestor's node.local in order.
    // Used by the Retarget Pose offset estimator (GUE) to get a bone's rest
    // position in world/component space for FindBetweenNormals-style
    // direction comparisons. Returns false (leaves *out untouched) if `name`
    // is not in node_map_.
    bool GetBindWorldTransform(const std::string& name, glm::mat4* out) const;

    // Standalone helper, same throwaway-parse pattern as
    // AllNodeNamesUnoptimized: opens `path` (any file sharing a naming
    // convention's rest pose, e.g. any Mixamo export) purely to extract every
    // node's bind-pose GLOBAL position, keyed by the same pipe-stripped name
    // convention used elsewhere for external files ("Armature|Hips" ->
    // "Hips"). Used by the offset estimator to get the SOURCE side of a
    // bone-direction comparison. Never touches any live Model.
    static std::unordered_map<std::string, glm::vec3> BindWorldPositionsUnoptimized(const char* path);

    // World (root-composed) bind-pose rotation of `name`'s PARENT node —
    // i.e. every ancestor's local rotation multiplied root-to-parent
    // (inclusive of the parent, exclusive of `name` itself). Identity if
    // `name`'s parent is the root (no parent). Used by the Retarget Pose
    // offset estimator: comparing the PARENT FRAME's full composed
    // orientation (not just a bone-direction vector) uses all available
    // rotational information and has no twist ambiguity, unlike
    // FindBetweenNormals on a single direction vector. Returns false if
    // `name` is not in node_map_.
    bool GetBindParentWorldRotation(const std::string& name, glm::quat* out) const;

    // Standalone helper, same throwaway-parse pattern as
    // AllNodeNamesUnoptimized/BindWorldPositionsUnoptimized: for EVERY node
    // in `path`'s own hierarchy (keyed by pipe-stripped name), returns the
    // WORLD (root-composed) bind-pose rotation of THAT NODE'S PARENT — the
    // origin-side counterpart to GetBindParentWorldRotation. Parent chains
    // are walked exactly as authored (including synthetic
    // "_$AssimpFbx$_..." nodes when one is the immediate parent — this is
    // deliberately NOT the same filtering as ComposedOriginBindRotation).
    // Never touches any live Model.
    static std::unordered_map<std::string, glm::quat> ParentBindWorldRotationsUnoptimized(const char* path);

    // Returns the bone's model-space world transform (global_inv_ * accumulated
    // hierarchy, WITHOUT inverse-bind) from the last Compute*Bones call.
    // Use this for attachment placement; the skinning matrix in out_mats has
    // the inverse-bind baked in and must NOT be used to position child objects.
    // Returns false — leaves *out untouched — if bone name is unknown.
    // NOTE: bone_world_transforms_ lives on the shared Model; do not call this
    // for the same model from two actors in the same frame (B4 will move this
    // per-actor when needed).
    bool GetBoneWorldTransform(const std::string& name, glm::mat4* out) const;

    // Fill out_mats[0..kMaxBones-1] with the final bone matrices for the
    // given clip at time_sec (loops). Upload to the bone SSBO before submit.
    // `mesh_idx` selects which submesh's bone_offsets[] to use — because in
    // multi-part models each submesh has its own mesh-local space and the
    // per-bone inverse-bind differs across parts.
    void ComputeBones(int clip_idx, float time_sec, int mesh_idx,
                      glm::mat4* out_mats, int max_out) const;

    // Like ComputeBones but blends two clips in LOCAL bone space before composing
    // the global hierarchy — this is the correct way to crossfade animations.
    // Blending the final skinning matrices (post-hierarchy) produces distortion
    // on deep bones (arms, fingers) because their matrices encode all accumulated
    // parent rotations. alpha=0 → fully from, alpha=1 → fully to.
    // Uses NLERP with antipodal fix for rotations, matching Unreal's FastLerp.
    void ComputeBlendedBones(int clip_from, float t_from,
                             int clip_to,   float t_to,
                             float alpha,   int mesh_idx,
                             glm::mat4* out_mats, int max_out) const;

    // Load animations from a separate file and append them to this model's
    // clip list. name_override replaces the clip name embedded in the file
    // (pass "" to keep the file's own name). Returns the index of the first
    // appended clip, or -1 on failure.
    int AppendAnimationsFrom(const char* path, const char* name_override = "");

    // Sets the retargeting fallback map (external bone name -> this model's
    // own bone/node name) consulted by AppendAnimationsFrom when a clip's
    // channel name has no direct match in node_map_. The map is plain DATA —
    // this class has no knowledge of "Mixamo" or any other naming convention;
    // the caller (GUE preview, or the game client applying a server-resolved
    // map) builds it however it wants and injects it here. Replaces any
    // previously set map. Pass an empty map to clear (restores exact-name-
    // only matching).
    void SetBoneAliases(std::unordered_map<std::string, std::string> aliases) {
        bone_aliases_ = std::move(aliases);
        ++bone_alias_revision_;
    }

    // Sets the calibratable Retarget Pose rotation-offset map (this model's
    // own bone/node name -> offset quaternion), consulted by
    // AppendAnimationsFrom for retargeted channels only (see the retargeting
    // formula in model.cpp). Plain DATA, same pattern as SetBoneAliases: this
    // class has no knowledge of "Mixamo" or model_retarget_offsets — the
    // caller builds the map from BuildBoneRetargetOffsetMap (db.go) or its
    // GUE-local mirror and injects it here. A bone absent from the map (or
    // an empty map altogether) is treated as identity offset — i.e. exactly
    // today's behavior, unchanged. Replaces any previously set map.
    void SetBoneRetargetOffsets(std::unordered_map<std::string, glm::quat> offsets) {
        bone_retarget_offsets_ = std::move(offsets);
        ++bone_alias_revision_;
    }

    // Current revision stamp — bumped by SetBoneAliases and
    // SetBoneRetargetOffsets. Exposed read-only so callers (GUE) can tell
    // whether a Model's already-baked clips are stale, e.g. to decide
    // whether to force a specific clip re-append instead of relying on
    // AppendAnimationsFrom's own (already revision-aware) fast path.
    int BoneAliasRevision() const { return bone_alias_revision_; }

    // Rename an embedded clip by its FBX-native name (e.g. "mixamo.com") to a
    // game action name (e.g. "Idle") so FindClip resolves it. No-op if not found.
    void AliasClip(const std::string& native_name, const std::string& new_name);

    // Replace the material of ALL submeshes with external PBR data. Useful
    // when the model file doesn't embed textures (e.g. FBX rigs shipped
    // without bundled textures) and the Media Actor Def provides them.
    // Empty paths skip that texture channel (keeps model's default / embedded).
    // Factors (albedo_rgb, roughness, metallic) are always applied.
    void OverrideMaterial(const std::string& albedo_path,
                          const std::string& normal_path,
                          const std::string& orm_path,
                          float albedo_r, float albedo_g, float albedo_b,
                          float roughness, float metallic);

    // Single PBR material's texture paths, used by ApplyMaterialsByName.
    // Paths may be absolute or relative to the current working directory;
    // empty strings mean "no texture for this channel".
    struct MaterialPaths {
        std::string albedo;
        std::string normal;
        std::string orm;   // packed: R=AO, G=roughness, B=metallic (glTF)
    };

    // Per-submesh material resolution by name. For each entry in `by_name`,
    // loads its textures from disk (once per entry, cached by name), registers
    // the resulting GL handles in `mm`, and assigns the returned index to
    // every submesh whose `material_name` matches. Submeshes whose names are
    // not found in the map keep their existing material_idx. Textures loaded
    // by this call are owned by the Model and freed in Destroy().
    void ApplyMaterialsByName(
        MaterialManager& mm,
        const std::unordered_map<std::string, MaterialPaths>& by_name);

    // Re-registers every embedded submesh in `mm` with blackCutout=enabled,
    // updating albedoFactor.w in the SSBO so the deferred gBuffer pass discards
    // near-black pixels (hair, foliage, fences). No-op if mm is null.
    void ApplyBlackCutout(bool enabled, MaterialManager* mm);

private:
    std::vector<SubMesh>       meshes_;
    std::string                directory_;
    std::string                model_path_; // full path passed to Load(); used in tex-resolve logs
    // Set to N in OverrideMaterial; Actor::Submit reads and decrements to log the next N draw
    // submissions without spamming every frame.
    mutable int                diag_log_submits_remaining_ = 0;
    float                      aabb_max_y_ = 0.f;
    glm::vec3                  aabb_min_   = glm::vec3( 1e30f);
    glm::vec3                  aabb_max_   = glm::vec3(-1e30f);

    std::vector<AnimNode>      anim_nodes_;
    std::map<std::string,int>  node_map_;
    std::vector<BoneInfo>      bones_;
    std::map<std::string,int>  bone_map_;
    std::vector<AnimClip>      clips_;
    // Retargeting fallback: external bone name -> this model's own node name.
    // Consulted only when node_map_.find(rc.name) fails in
    // AppendAnimationsFrom (model.cpp). Empty by default — exact-name
    // matching is unaffected until a caller opts in via SetBoneAliases.
    std::unordered_map<std::string, std::string> bone_aliases_;
    // Calibratable Retarget Pose rotation offset per bone (this model's own
    // node name -> offset quaternion). Empty by default (== identity for
    // every bone, i.e. no behavior change) until a caller opts in via
    // SetBoneRetargetOffsets. See AppendAnimationsFrom's retargeting formula.
    std::unordered_map<std::string, glm::quat> bone_retarget_offsets_;
    // Bumped by SetBoneAliases/SetBoneRetargetOffsets — see AnimClip::
    // built_with_alias_revision and AppendAnimationsFrom's name-match guard.
    int bone_alias_revision_ = 0;
    // World transform per bone (global_inv_ * global[ni], no inverse-bind).
    // Sized to bones_.size() in Load; updated each Compute*Bones call.
    // mutable: Compute*Bones are const but write this as a pose cache.
    mutable std::vector<glm::mat4> bone_world_transforms_;
    glm::mat4                  global_inv_    = glm::mat4(1.f);
    glm::vec3                  hips_bind_pos_ = glm::vec3(0.f);
    bool                       skinned_       = false;

    // GL textures created by ApplyMaterialsByName — we own them, destroy in Destroy().
    std::vector<GLuint>        owned_textures_;

    // Per-model UV transform override loaded from a "<path>.uv" sidecar file.
    // Replaces the Assimp-detected KHR_texture_transform when active.
    glm::vec2 uv_sidecar_offset_ = glm::vec2(0.f, 0.f);
    glm::vec2 uv_sidecar_scale_  = glm::vec2(1.f, 1.f);
    bool      uv_sidecar_active_ = false;
    bool      source_is_gltf_    = false;

    // Post-flip UV transform actually applied to the VBO data — exposed via
    // EffectiveUVOffset/Scale so the GUE can compose live slider deltas
    // against the real baseline when writing a new sidecar.
    glm::vec2 uv_effective_offset_ = glm::vec2(0.f, 0.f);
    glm::vec2 uv_effective_scale_  = glm::vec2(1.f, 1.f);

    void    BuildNodeTree(aiNode* node, int parent_idx);
    void    LoadAnimations(const aiScene* scene);
    void    ProcessNode(aiNode* node, const aiScene* scene,
                        std::vector<std::vector<glm::ivec4>>& bids,
                        std::vector<std::vector<glm::vec4>>&  bwts,
                        const glm::mat4& parent_xform);
    SubMesh ProcessMesh(aiMesh* mesh, const aiScene* scene,
                        std::vector<glm::ivec4>& bids,
                        std::vector<glm::vec4>&  bwts,
                        const glm::mat4& node_transform);
    GLuint  LoadTex(const aiScene* scene, const std::string& path, bool srgb) const;
    void    GeneratePlaceholder();
    static void FreeMesh(SubMesh& m);

    friend ConsolidationResult ConsolidateMeshes(Model& model, const char* path);
};

} // namespace rco::renderer
