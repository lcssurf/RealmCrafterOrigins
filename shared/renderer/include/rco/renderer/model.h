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
    std::vector<BoneChannel>  channels;
    std::map<std::string,int> chan_map; // node name → channels[] index
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

class Model {
public:
    ~Model();

    bool log_bones = false;  // enable verbose load/animation diagnostics

    // Load geometry + textures from `path`. If `mm` is provided, each submesh's
    // textures are also registered in the MaterialManager and `material_idx`
    // is written back so the submesh can be drawn via the deferred pipeline
    // without a hardcoded material slot.
    bool Load(const char* path, MaterialManager* mm = nullptr);
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

    bool  HasAnimations()       const { return !clips_.empty(); }
    int   ClipCount()           const { return (int)clips_.size(); }
    const std::string& ClipName(int i)     const { return clips_[i].name; }
    float ClipDuration(int i)   const {
        return (i >= 0 && i < (int)clips_.size()) ? clips_[i].duration_sec : 0.f;
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

private:
    std::vector<SubMesh>       meshes_;
    std::string                directory_;
    float                      aabb_max_y_ = 0.f;
    glm::vec3                  aabb_min_   = glm::vec3( 1e30f);
    glm::vec3                  aabb_max_   = glm::vec3(-1e30f);

    std::vector<AnimNode>      anim_nodes_;
    std::map<std::string,int>  node_map_;
    std::vector<BoneInfo>      bones_;
    std::map<std::string,int>  bone_map_;
    std::vector<AnimClip>      clips_;
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
