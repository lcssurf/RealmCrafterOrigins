#pragma once
#include <glm/glm.hpp>
#include <string>
#include <array>
#include <vector>
#include <unordered_map>
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
    // Init an actor from a file. If `mm` is provided, the model's textures are
    // also registered in the MaterialManager so the deferred pipeline can
    // shade them correctly (caller must RebuildMaterialsBuffer afterwards).
    void Init(const char* shader_dir, const char* model_path,
              MaterialManager* mm = nullptr);

    // Load animations from a separate file.
    // name: clip name used for PlayAnim(); "" keeps the file's embedded name.
    void LoadAnim(const char* path, const char* name = "");

    // Override the model's material with external PBR textures. Passes through
    // to Model::OverrideMaterial; safe to call any time after Init().
    void OverrideMaterial(const std::string& albedo_path,
                          const std::string& normal_path,
                          const std::string& orm_path,
                          float albedo_r, float albedo_g, float albedo_b,
                          float roughness, float metallic) {
        model_.OverrideMaterial(albedo_path, normal_path, orm_path,
                                albedo_r, albedo_g, albedo_b,
                                roughness, metallic);
    }

    // Apply per-aiMaterial-name textures (Substance-style "blinn1"/"ID01"
    // mapping). Delegates to Model::ApplyMaterialsByName and uses the passed
    // MaterialManager.
    void ApplyMaterialsByName(MaterialManager& mm,
        const std::unordered_map<std::string, Model::MaterialPaths>& by_name) {
        model_.ApplyMaterialsByName(mm, by_name);
    }

    // Names of the distinct aiMaterials this actor's model references.
    std::vector<std::string> MaterialNames() const { return model_.MaterialNames(); }

    // True once Init() successfully loaded a model.
    bool IsLoaded() const { return model_.IsLoaded(); }

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

    // Like Submit(), but the caller supplies the object-to-world matrix.
    // Bypasses position/yaw/scale so full Euler rotation + anisotropic
    // scaling are supported (e.g. scenery props with pitch/roll/non-uniform
    // scale). Uses the actor's internal anim state.
    void SubmitWithMatrix(Pipeline& pipeline, const glm::mat4& model_matrix);

    void Destroy();

    glm::vec3 position = {0.f, 0.f, 0.f};
    float     yaw      = 0.f;
    float     scale    = 1.f;

    const std::string& CurrentAnim() const { return cur_name_; }
    float              AnimTime()    const { return anim_t_; }

    // World-space height of the model (bind-pose AABB top × scale).
    // Returns a sensible default (1.8) if the model isn't loaded yet.
    float ModelHeight() const {
        float h = model_.MaxY() * scale;
        return h > 0.1f ? h : 1.8f;
    }

    // Read-only access to the wrapped Model — useful for callers that need
    // its clip metadata or submesh material names.
    const Model& model() const { return model_; }

private:
    Model  model_;

    std::string cur_name_;     // currently playing clip name
    int         clip_idx_ = -1;
    float       anim_t_   = 0.f;
    bool        loop_     = true;
    std::string return_to_;    // clip to play after one-shot ends

    // One SSBO per submesh — each submesh carries its own bind-pose offsets
    // so their bone matrices differ. Pipeline submissions are deferred and
    // all sample their SSBO at End(), so we can't reuse a single buffer
    // across multiple submissions in the same frame.
    std::vector<unsigned int> bone_ssbos_;   // sized to model_.meshes().size()

    int  FindClip(const std::string& name) const;
    void EnsureBoneSSBOs_(size_t count);
    void UploadBonesToSSBO_(size_t mesh_idx, const glm::mat4* bones, int count);
};

} // namespace rco::renderer
