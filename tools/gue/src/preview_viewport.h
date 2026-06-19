#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <rco/renderer/actor.h>
#include <rco/renderer/model.h>
#include <string>
#include <vector>
#include <array>
#include <functional>

struct ImVec2;

namespace rco::renderer { class Engine; class Pipeline; }

namespace gue {

// Lightweight descriptor of one configured action from the actor def's anim map.
// Passed each frame from DrawActorDefs so the preview dropdown lists actions
// (Idle, Walk, Attack…) instead of raw clip names.
// action_index is the position of this entry in editActorDef_.anim_map; Set
// Start / Set End resolve the write through callbacks at click time (not via
// element pointers) so realloc of anim_map never produces dangling access.
// start_frame / end_frame are copied fresh each frame so table edits reach
// the preview without requiring a dropdown re-selection.
struct AnimActionEntry {
    std::string action;
    std::string source_path;   // "" = embedded in body; else separate anim file
    std::string clip_override; // "" = use action as clip name; else explicit name
    bool        loop         = true;
    int         action_index = -1;  // index into editActorDef_.anim_map
    int         start_frame  = 0;   // first frame of the playback range (0 = clip start)
    int         end_frame    = -1;  // last frame (-1 = play to end of clip)
};

// 3D preview panel rendered via the shared deferred renderer (Engine + Pipeline).
// Owns a single rco::renderer::Actor — the same class the client and Zone
// editor use — so every fix to model loading, skinning, material mapping
// etc. lands in one place and is picked up everywhere.
class PreviewViewport {
public:
    struct CollisionShape {
        int   type     = 0; // 0=box, 1=sphere, 2=mesh, 3=wedge
        float offset_x = 0.f, offset_y = 0.f, offset_z = 0.f;
        float size_x   = 1.f, size_y   = 1.f, size_z   = 1.f;
        float detail_a = 0.f;
        float detail_b = 0.f;
    };

    ~PreviewViewport();

    void Init(rco::renderer::Engine*   engine,
              rco::renderer::Pipeline* pipeline);

    bool LoadModel(const std::string& path);

    // Drop any cached version of `path` and reload from disk. Used after the
    // user edits the per-model UV transform so the sidecar is re-read.
    void ReloadCurrent();

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
    void SetActorScale(float s) {
        actor_scale_ = s > 0.f ? s : 1.f;
        actor_.scale = actor_scale_;
    }

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

    // Called each frame from DrawActorDefs before DrawImGui().
    // Replaces the raw clip dropdown with a dropdown of configured actions.
    // on_set_start(action_index, frame) / on_set_end(action_index, frame) are
    // called when the dev clicks Set Start / Set End; media.cpp provides lambdas
    // that capture editActorDef_ by this-pointer and resolve [action_index] at
    // click time — robust to any realloc of anim_map between frames.
    void SetAnimActions(std::vector<AnimActionEntry> actions,
                        std::function<void(int,int)>  on_set_start,
                        std::function<void(int,int)>  on_set_end);

    void DrawImGui();
    void Clear();
    const std::string& CurrentPath() const { return current_path_; }

    // Read-only access to the loaded model for diagnostic display in the
    // calling tab (e.g. checking which submeshes have missing albedo textures).
    const rco::renderer::Model& GetModel() const { return actor_.model(); }

    // Per-model UV transform overlay edited via the on-viewport sliders.
    // Values are post-flip and apply directly: u_out = u * scale + offset.
    void SetUVTransform(float ox, float oy, float sx, float sy) {
        uv_offset_[0] = ox; uv_offset_[1] = oy;
        uv_scale_[0]  = sx; uv_scale_[1]  = sy;
    }
    void GetUVTransform(float& ox, float& oy, float& sx, float& sy) const {
        ox = uv_offset_[0]; oy = uv_offset_[1];
        sx = uv_scale_[0];  sy = uv_scale_[1];
    }

    void SetCollisionShapes(const std::vector<CollisionShape>& shapes) {
        collision_shapes_ = shapes;
    }
    void SetCollisionPreviewVisible(bool v) { show_collision_preview_ = v; }
    bool CollisionPreviewVisible() const { return show_collision_preview_; }

private:
    void RenderToEngineFrame_(int w, int h, float dt);
    void DrawCollisionOverlay_(const ImVec2& image_pos, const ImVec2& image_size) const;
    void EnsureCollisionMeshCache_() const;
    // Resolve and play the clip for a configured action entry.
    void PlayActionEntry_(const AnimActionEntry& e);

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

    float anim_t_        = 0.f;
    bool  playing_       = true;
    float sun_intensity_ = 1.0f;
    float actor_scale_   = 1.0f;

    // Configured actions from the actor def's anim map. Refreshed each frame
    // from DrawActorDefs via SetAnimActions().
    std::vector<AnimActionEntry>  anim_actions_;
    int                           sel_action_      = -1;
    std::string                   sel_action_name_;  // stable across SetAnimActions calls
    std::function<void(int,int)>  on_set_start_;     // (action_index, frame) → anim_map[idx].start_frame
    std::function<void(int,int)>  on_set_end_;       // (action_index, frame) → anim_map[idx].end_frame

    // Clip range state (A1, B2–B4). Resolved in PlayActionEntry_ and refreshed
    // each frame from the selected AnimActionEntry so table edits propagate live.
    int   cur_clip_idx_   = -1;   // actual clip index in model (not name-based)
    float clip_start_sec_ = 0.f;  // start of playback range in seconds
    float clip_end_sec_   = 0.f;  // end of playback range (0 if full clip)

    // UV diagnostic — apply offset/scale on top of the VBO's UVs so the user
    // can confirm the right alignment when the import didn't pick up the
    // material's KHR_texture_transform.
    float uv_offset_[2] = {0.f, 0.f};
    float uv_scale_[2]  = {1.f, 1.f};

    // Simple forward FBO used for non-skinned (static) actor preview.
    // Bypasses the bindless deferred pipeline entirely so plain sampler2D
    // textures work on all drivers.
    GLuint simple_fbo_   = 0;
    GLuint simple_color_ = 0;
    GLuint simple_depth_ = 0;
    int    simple_w_     = 0;
    int    simple_h_     = 0;

    void EnsureSimpleFbo_(int w, int h);

    // Debounce Engine::Resize when the user drags the splitter.
    int last_w_        = 0, last_h_ = 0;
    int stable_frames_ = 0;

    bool                        show_collision_preview_ = false;
    std::vector<CollisionShape> collision_shapes_;
    mutable std::vector<std::array<glm::vec3, 3>> collision_mesh_tris_;
    mutable std::string                         collision_mesh_cache_path_;
    mutable std::vector<std::array<glm::vec3, 3>> collision_mesh_simplified_tris_;
    mutable std::string                           collision_mesh_simplified_path_;
    mutable float                                 collision_mesh_simplified_budget_ = -1.f;
};

} // namespace gue
