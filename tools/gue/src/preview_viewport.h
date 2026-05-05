#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <rco/renderer/actor.h>
#include <rco/renderer/model.h>
#include <string>
#include <vector>

namespace rco::renderer { class Engine; class Pipeline; }

namespace gue {

// 3D preview panel rendered via the shared deferred renderer (Engine + Pipeline).
// Owns a single rco::renderer::Actor — the same class the client and Zone
// editor use — so every fix to model loading, skinning, material mapping
// etc. lands in one place and is picked up everywhere.
class PreviewViewport {
public:
    ~PreviewViewport();

    void Init(rco::renderer::Engine*   engine,
              rco::renderer::Pipeline* pipeline);

    bool LoadModel(const std::string& path);

    // Names of the aiMaterials the currently-loaded model references
    // (distinct, in order of first occurrence). Empty if no model loaded.
    std::vector<std::string> MaterialNames() const {
        return actor_.IsLoaded() ? actor_.MaterialNames()
                                 : std::vector<std::string>();
    }

    void OverrideMaterial(const std::string& albedo,
                          const std::string& normal,
                          const std::string& orm,
                          float albedo_r, float albedo_g, float albedo_b,
                          float roughness, float metallic);

    // Actor-level scale multiplier. Multiplies each submesh's model scale
    // live — used by the Actor Def editor to preview size overrides
    // (filhote/pai grandão). Default 1.0.
    void SetActorScale(float s) { actor_.scale = s > 0.f ? s : 1.f; }

    // After LoadModel, resolve each submesh's aiMaterial name against the
    // MediaTab's materials list and bind real PBR textures from disk.
    // Does nothing if the model or MediaTab materials are unavailable.
    struct MaterialLookup {
        std::string name;
        std::string albedo_rel;
        std::string normal_rel;
        std::string orm_rel;
    };
    void ApplyMaterialsFromMedia(const std::vector<MaterialLookup>& mats);

    void DrawImGui();
    void Clear();
    const std::string& CurrentPath() const { return current_path_; }

private:
    void RenderToEngineFrame_(int w, int h, float dt);

    rco::renderer::Engine*   engine_   = nullptr;
    rco::renderer::Pipeline* pipeline_ = nullptr;

    rco::renderer::Actor  actor_;
    std::string           current_path_;

    // Orbit camera
    float     cam_yaw_   = 0.f;
    float     cam_pitch_ = -15.f;
    float     cam_dist_  = 2.5f;
    glm::vec3 cam_target_{0.f, 1.f, 0.f};
    float     cam_near_     = 0.05f;
    float     cam_far_      = 200.f;
    float     cam_dist_min_ = 0.1f;
    float     cam_dist_max_ = 50.f;

    // Resets orbit camera to frame the currently loaded model's AABB.
    // Called automatically by LoadModel on success.
    void FitCameraToModel();

    float anim_t_  = 0.f;
    bool  playing_ = true;

    // Debounce Engine::Resize when the user drags the splitter.
    int last_w_        = 0, last_h_ = 0;
    int stable_frames_ = 0;
};

} // namespace gue
