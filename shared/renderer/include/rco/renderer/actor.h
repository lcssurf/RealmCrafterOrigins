#pragma once
#include <glm/glm.hpp>
#include <string>
#include <array>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include "rco/renderer/model.h"

namespace rco::renderer {

class Pipeline;
class MaterialManager;

// Renderable skeletal model instance used by every tool/viewport in the
// project. Owns one Model (geometry + clips + textures) and the per-submesh
// bone SSBOs required to drive Pipeline::SubmitSkinned. The Pipeline defers
// submits to End(), so every submesh needs its own SSBO when bone offsets
// differ per submesh (multi-part meshes from Substance Painter, Mixamo, etc).
//
// Callers position/rotate/scale via the public fields; drive animation via
// PlayAnim() + Update() for active game actors, or via SubmitAs() when the
// animation state is owned externally (zone previews, UI pickers). For
// static scenery, leave clip_idx at -1 and only call Submit().
class Actor {
public:
    enum class ReadabilityProfile : uint8_t {
        World = 0,
        Character = 1,
    };

    // Init an actor from a file. If `mm` is provided, the model's textures are
    // also registered in the MaterialManager so the deferred pipeline can
    // shade them correctly (caller must RebuildMaterialsBuffer afterwards).
    void Init(const char* shader_dir, const char* model_path,
              MaterialManager* mm = nullptr);

    ~Actor() { Destroy(); }

    // Load animations from a separate file.
    // name: clip name used for PlayAnim(); "" keeps the file's embedded name.
    void LoadAnim(const char* path, const char* name = "");

    // Rename an embedded clip by its native FBX name to a game action name.
    // Needed when the body model contains the animation under a name like
    // "mixamo.com" and the Actor Def maps it to "Idle".
    void AliasClip(const std::string& native_name, const std::string& action_name) {
        if (model_) model_->AliasClip(native_name, action_name);
    }

    // Override the model's material with external PBR textures. Passes through
    // to Model::OverrideMaterial; safe to call any time after Init().
    void OverrideMaterial(const std::string& albedo_path,
                          const std::string& normal_path,
                          const std::string& orm_path,
                          float albedo_r, float albedo_g, float albedo_b,
                          float roughness, float metallic) {
        if (model_) model_->OverrideMaterial(albedo_path, normal_path, orm_path,
                                albedo_r, albedo_g, albedo_b,
                                roughness, metallic);
    }

    // Apply per-aiMaterial-name textures (Substance-style "blinn1"/"ID01"
    // mapping). Delegates to Model::ApplyMaterialsByName and uses the passed
    // MaterialManager.
    void ApplyMaterialsByName(MaterialManager& mm,
        const std::unordered_map<std::string, Model::MaterialPaths>& by_name) {
        if (model_) model_->ApplyMaterialsByName(mm, by_name);
    }

    // Names of the distinct aiMaterials this actor's model references.
    std::vector<std::string> MaterialNames() const {
        return model_ ? model_->MaterialNames() : std::vector<std::string>{};
    }

    // True once Init() successfully loaded a model.
    bool IsLoaded() const { return model_ && model_->IsLoaded(); }

    // Play an animation by name.
    //   loop     : true = loop forever; false = play once then switch to return_to
    //   return_to: clip name to switch to after a one-shot finishes ("" = keep last)
    //   restart  : force restart even if the same clip is already playing
    void PlayAnim(const std::string& name,
                  bool loop          = true,
                  const std::string& return_to = "",
                  bool restart       = false);

    void Update(float dt);

    // Submit this actor to the deferred pipeline for this frame.
    // Uses the current cur_name_/anim_t_ (updated by Update()).
    void Submit(Pipeline& pipeline);

    // Submit using external animation state (for shared actor instances).
    // Does not modify this actor's playback state.
    void SubmitAs(const std::string& anim_name, float anim_t, bool loop,
                  Pipeline& pipeline);

    // Submit with SLERP-blended pose between two clips.
    // blend_alpha: 0.0 = fully from_anim, 1.0 = fully to_anim (smoothstep already applied by caller).
    void SubmitBlended(Pipeline& pipeline,
                       const std::string& from_anim, float from_t,
                       const std::string& to_anim,   float to_t,
                       float blend_alpha);

    // Like Submit(), but the caller supplies the object-to-world matrix.
    // Bypasses position/yaw/scale so full Euler rotation + anisotropic
    // scaling are supported (e.g. scenery props with pitch/roll/non-uniform
    // scale). Uses the actor's internal anim state.
    void SubmitWithMatrix(Pipeline& pipeline, const glm::mat4& model_matrix);

    void Destroy();

    void SetReadabilityProfile(ReadabilityProfile profile) {
        readability_profile_ = profile;
    }
    ReadabilityProfile GetReadabilityProfile() const { return readability_profile_; }

    glm::vec3 position   = {0.f, 0.f, 0.f};
    float     yaw        = 0.f;
    float     scale      = 1.f;
    float     yaw_offset = 0.f;  // extra Y rotation (deg) applied before world yaw
    float     y_offset   = 0.f;  // vertical offset (world units) added to position

    const std::string& CurrentAnim()  const { return cur_name_; }
    float              AnimTime()     const { return anim_t_; }
    float ClipDuration(const std::string& name) const {
        int idx = FindClip(name);
        return (model_ && idx >= 0) ? model_->ClipDuration(idx) : 0.f;
    }

    // World-space height of the model (bind-pose AABB full extent × scale).
    // Uses BoundsMax.y - BoundsMin.y so models whose origin is not at the feet
    // (center-origin exports) report the correct full visual height.
    // Returns a sensible default (1.8) if the model isn't loaded yet.
    float ModelHeight() const {
        float h = model_ ? (model_->BoundsMax().y - model_->BoundsMin().y) * scale : 0.f;
        return h > 0.1f ? h : 1.8f;
    }

    // World-space Y offset from position.y to the model's visual feet.
    // = y_offset + BoundsMin.y * scale
    // BoundsMin may be positive (model exported above its local origin),
    // negative (cape/skirt geometry below feet), or zero.
    // This is the only value that reliably anchors camera maths to the
    // actual rendered foot position.
    float FeetOffset() const {
        float min_y = model_ ? model_->BoundsMin().y * scale : 0.f;
        return y_offset + min_y;
    }

    // Returns the bone's model-space world transform from the last Submit* call.
    // Multiply by the actor's own model matrix to get world-space placement.
    // See Model::GetBoneWorldTransform for the per-bone-sharing caveat.
    bool GetBoneWorldTransform(const std::string& name, glm::mat4* out) const {
        return model_ ? model_->GetBoneWorldTransform(name, out) : false;
    }

    // Returns all bone names in this actor's model (alphabetical order).
    // Used by the GUE socket editor to populate bone dropdowns.
    std::vector<std::string> BoneNames() const {
        return model_ ? model_->BoneNames() : std::vector<std::string>{};
    }

    // Read-only access to the wrapped Model — useful for callers that need
    // its clip metadata or submesh material names.
    const Model& model() const {
        static Model s_empty;
        return model_ ? *model_ : s_empty;
    }

    // Cross-fade blend duration (seconds). Tweak per-actor if needed.
    float blend_dur = 0.15f;

private:
    std::shared_ptr<Model> model_;

    std::string cur_name_;     // currently playing clip name
    int         clip_idx_ = -1;
    float       anim_t_   = 0.f;
    bool        loop_     = true;
    std::string return_to_;    // clip to play after one-shot ends

    // Cross-fade state — valid while blend_t_ < blend_dur_
    std::string from_name_;
    int         from_clip_idx_ = -1;
    float       from_t_        = 0.f;
    float       blend_t_       = 1.f;  // starts >= blend_dur so no blend initially

    // One SSBO per submesh — each submesh carries its own bind-pose offsets
    // so their bone matrices differ. Pipeline submissions are deferred and
    // all sample their SSBO at End(), so we can't reuse a single buffer
    // across multiple submissions in the same frame.
    std::vector<unsigned int> bone_ssbos_;   // sized to model_->meshes().size()
    ReadabilityProfile readability_profile_ = ReadabilityProfile::World;

    int  FindClip(const std::string& name) const;
    void EnsureBoneSSBOs_(size_t count);
    void UploadBonesToSSBO_(size_t mesh_idx, const glm::mat4* bones, int count);
    float ReadabilityMask_() const {
        return readability_profile_ == ReadabilityProfile::Character ? 1.0f : 0.0f;
    }
    // Fill `out[0..max-1]` with SLERP-blended bone matrices for mesh_idx.
    void FillBlendedBones_(int cidx_from, float ft, int cidx_to, float tt,
                           float alpha, int mesh_idx,
                           glm::mat4* out, int max) const;
};

} // namespace rco::renderer
