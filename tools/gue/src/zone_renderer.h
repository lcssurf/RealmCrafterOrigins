#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <rco/renderer/actor.h>
#include <rco/renderer/model.h>
#include "zone_scene.h"
#include "terrain/editable_terrain.h"

namespace rco::renderer { class Engine; class Pipeline; }

namespace gue { struct ZoneCamera; }

namespace gue {

// Selection object types — used to distinguish what selectedID refers to.
enum ZSelType {
    kSelNone      = -1,
    kSelPortal    = 0,
    kSelTrigger   = 1,
    kSelSoundZone = 2,
    kSelColBox    = 3,
    kSelWaypoint  = 4,
    kSelNpc       = 5,
    kSelEmitter   = 6,
    kSelWater     = 7,
    kSelScenery   = 8,
};

// Off-screen FBO renderer for the zone editor viewport.
// Each phase adds rendering for its object types.
class ZoneRenderer {
public:
    ~ZoneRenderer();

    // Call once after the GL context is ready.
    bool Init();

    // Called whenever the viewport size changes.
    void Resize(int w, int h);

    // Render one frame. Each phase adds its draw calls.
    void RenderFrame(const ZoneCamera& cam,
                     const ZoneScene&  scene,
                     int selectedID   = -1,
                     int selectedType = kSelNone,
                     float             dt = 1.f/60.f);

    // The finished colour texture — pass as ImTextureID to ImGui::Image().
    // UV must be flipped: uv0=(0,1), uv1=(1,0) because OpenGL is bottom-up.
    ImTextureID GetTexture() const;

    int width()  const { return w_; }
    int height() const { return h_; }

    // Viewport shading mode — "Simple" uses the GUE's forward shader, "PBR"
    // runs the full client deferred pipeline (Engine/Pipeline). PBR is
    // initialised lazily on first enable — cheap when left off.
    enum RenderMode { kRenderSimple = 0, kRenderPBR = 1 };
    void       SetRenderMode(RenderMode m);
    RenderMode renderMode() const { return renderMode_; }

    // Terrain backdrop — now an EditableTerrain owned by the renderer.
    void LoadTerrain(const std::string& areaName);
    EditableTerrain& terrain() { return terrain_; }
    const EditableTerrain& terrain() const { return terrain_; }

    // Draw a Unity-style move gizmo at `pos` — three axis arrows (X=red,
    // Y=green, Z=blue). `highlightAxis` in [0,2] highlights one axis in yellow
    // while dragging. `camPos` is used to keep gizmo screen-size roughly constant.
    void DrawMoveGizmo(const glm::vec3& pos, const glm::mat4& vp,
                       const glm::vec3& camPos, int highlightAxis = -1);

    // 3 rings (one per axis, perpendicular to it).
    void DrawRotateGizmo(const glm::vec3& pos, const glm::mat4& vp,
                         const glm::vec3& camPos, int highlightAxis = -1,
                         unsigned allowAxes = 0b111);

    // 3 cubes at the tip of each axis + a centre cube for uniform scale.
    void DrawScaleGizmo(const glm::vec3& pos, const glm::mat4& vp,
                        const glm::vec3& camPos, int highlightAxis = -1,
                        unsigned allowAxes = 0b111);

    // Axis length in world units at a given object position — matches the
    // gizmo so picking code can use the same length.
    static float GizmoAxisLength(const glm::vec3& objPos, const glm::vec3& camPos);

    // Gizmo state set by the tab before RenderFrame. Drawn inside the
    // deferred pipeline's forward pass so it respects scene depth and lands
    // on the same FBO as the rest of the frame.
    enum GizmoMode { kGizmoNone = 0, kGizmoMove, kGizmoRotate, kGizmoScale };
    struct GizmoState {
        GizmoMode mode        = kGizmoNone;
        glm::vec3 pos         = {};
        int       axis        = -1;       // active / highlighted
        unsigned  allow_axes  = 0b111;
    };
    void SetGizmo(const GizmoState& g) { gizmo_ = g; }

    // Binding info passed per model when syncing scene caches. `file_path`
    // is relative to dist/client/ (or "" to skip). `material_map` is the
    // aiMaterial-name → MaterialPaths mapping configured by the user in the
    // Media tab; applied via Model::ApplyMaterialsByName after Load so the
    // Lit-mode deferred pipeline renders with the correct textures.
    struct ModelBind {
        std::string file_path;
        std::unordered_map<std::string, rco::renderer::Model::MaterialPaths> material_map;
        // Actor-level multiplier applied on top of the model's import scale
        // at draw time. 1.0 = natural. Set from ActorDef.scale by the caller.
        float       actor_scale = 1.f;

        // Optional global material override, applied AFTER material_map.
        // Mirrors the Media preview behaviour: the Actor Def's per-slot
        // material_id paints every submesh of the slot when set. Empty
        // path means "no override".
        rco::renderer::Model::MaterialPaths material_override;
        // PBR factors that go alongside the override (only used when
        // material_override is set).
        float ovr_albedo_r = 1.f, ovr_albedo_g = 1.f, ovr_albedo_b = 1.f;
        float ovr_roughness = 0.5f, ovr_metallic = 0.f;
        bool  has_override  = false;
    };

    // Scenery mesh cache. Loads every referenced model, applies any provided
    // material mapping, and keeps the rest of the cache untouched.
    void SyncSceneryModels(const std::vector<ZScenery>& scenery,
                           const std::unordered_map<int, ModelBind>& binds);

    // Per-NPC body mesh cache. Keyed by NPC id (not model id) because two
    // NPCs can share the same actor_def but display different materials
    // through the per-model material_map configured in Media.
    void SyncNpcModels(const std::unordered_map<int, ModelBind>& npcBinds);

    // Phase 2 — primitive objects (sphere, box, cylinder, lines).
    // DrawSphere / DrawBox / DrawLine are called from RenderFrame internally.

private:
    // ── FBO ──────────────────────────────────────────────────────────────
    void BuildFBO(int w, int h);
    void DestroyFBO();

    GLuint fbo_      = 0;
    GLuint colorTex_ = 0;
    GLuint depthRbo_ = 0;
    int    w_ = 0, h_ = 0;

    // ── Primitive shader (uniform colour + MVP) ───────────────────────────
    bool   InitPrimShader();
    GLuint primProg_  = 0;
    bool   primsReady_ = false;

    // ── Primitive VAOs (generated procedurally) ───────────────────────────
    void BuildPrimitiveVAOs();
    void DrawSphere(const glm::vec3& pos, float r, const glm::vec4& col, const glm::mat4& vp);
    void DrawBox   (const glm::vec3& pos, const glm::vec3& scale, const glm::vec4& col, const glm::mat4& vp);
    void DrawLine  (const glm::vec3& a,   const glm::vec3& b,     const glm::vec4& col, const glm::mat4& vp);
    void DrawCircleXZ(const glm::vec2& xz, float r, float y, const glm::vec4& col, const glm::mat4& vp);

    GLuint sphereVAO_ = 0, sphereVBO_ = 0, sphereEBO_ = 0;
    int    sphereIdx_ = 0;
    GLuint boxVAO_    = 0, boxVBO_    = 0, boxEBO_    = 0;
    int    boxIdx_    = 0;
    GLuint lineVAO_   = 0, lineVBO_   = 0;

    // ── Terrain (editable, chunked, splatmap-blended) ─────────────────────
    EditableTerrain terrain_;
    std::string     loadedArea_;

    // ── Stubs kept for future shader work (unused) ────────────────────────

    // ── Scene actor cache ──────────────────────────────────────────────────
    // Every rendered scenery instance and NPC is a full-featured Actor —
    // same class Media preview and client use. Skinning, materials, and
    // submesh iteration all flow through rco::renderer::Actor::Submit(),
    // so fixes to model loading land everywhere at once.
    // Key: ZScenery.id (one actor per instance) / ZNpcSpawn.id.
    std::unordered_map<int, std::unique_ptr<rco::renderer::Actor>> sceneryActors_;
    std::unordered_map<int, std::unique_ptr<rco::renderer::Actor>> npcActors_;
    // Per-NPC actor-level scale (from ActorDef.scale). Kept in parallel with
    // npcActors_ so RenderFramePBR_ can apply it per-draw without having to
    // re-query Media on every frame.
    std::unordered_map<int, float> npcActorScale_;
    // Memo of which model_id a scenery actor is currently loaded with, so we
    // only rebuild (Destroy + Init) when the binding changes.
    std::unordered_map<int, int> sceneryActorModelId_;

    // Monotonic playback clock for NPC idle loops. Accumulated from dt at
    // every RenderFrame; shared across all NPC actors.
    float      elapsed_time_ = 0.f;
    glm::vec3  last_cam_pos_ = {};
    GizmoState gizmo_;

    // ── PBR render path (lazy) ────────────────────────────────────────────
    RenderMode                                renderMode_ = kRenderSimple;
    std::unique_ptr<rco::renderer::Engine>    fullEngine_;
    std::unique_ptr<rco::renderer::Pipeline>  fullPipeline_;
    void      EnsurePBR_(int w, int h);
    void      RenderFramePBR_   (const ZoneCamera&, const ZoneScene&, int selID, int selType, float dt);
    void      DrawForwardOverlays_(const ZoneScene&, int selID, int selType, const glm::mat4& vp);
};

} // namespace gue
