#pragma once
#include "../zone_camera.h"
#include "../zone_renderer.h"
#include "../zone_scene.h"
#include "../thumbnail_cache.h"

struct GLFWwindow;

#include <imgui.h>
#include <sqlite3.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace rco::renderer { class Engine; class Pipeline; }

namespace gue {
class MediaTab;

// ── Zone editor placement mode (what right-click creates) ────────────────────
enum ZoneMode {
    kModeScenery   = 0,
    kModeTerrain   = 1,
    kModeEmitters  = 2,
    kModeWater     = 3,
    kModeColBox    = 4,
    kModeSoundZone = 5,
    kModeTrigger   = 6,
    kModeWaypoint  = 7,
    kModePortal    = 8,
    kModeNPC        = 9,
    kModeEnviro     = 10,
    kModeOther      = 11,
    kModeSpawnPoint = 12,
    kModeColSphere  = 13,
    kModeCount      = 14,
};

// ── Lightweight cache entry for the terrain material picker ─────────────────
struct TerrainMediaMat {
    int         id              = 0;
    std::string name;
    std::string albedo_path;   // relative to dist/ root
    std::string normal_path;
    std::string orm_path;
    float       normal_strength = 2.5f;
};

// ── Gizmo / transform mode ───────────────────────────────────────────────────
enum XFormMode {
    kXFormSelect = 0,
    kXFormMove   = 1,
    kXFormRotate = 2,
    kXFormScale  = 3,
};

class ZonesTab {
public:
    void SetRenderer(::rco::renderer::Engine* engine,
                     ::rco::renderer::Pipeline* pipeline) {
        (void)engine; (void)pipeline;
    }

    void SetWindow(GLFWwindow* win) { thumbs_.Init(win); }

    // Called every ImGui frame when the Zones tab is active.
    void Draw(sqlite3* db, MediaTab* media);

private:
    // ── Layout sections (new design) ─────────────────────────────────────────
    void DrawTopBar      (sqlite3* db);
    void DrawSceneSidebar(sqlite3* db, MediaTab* media);
    void DrawViewport    (sqlite3* db, MediaTab* media);  // fills parent child
    void DrawFloatingToolbar();                            // overlaid inside viewport
    void DrawInspector   (sqlite3* db, MediaTab* media);
    void DrawStatusBar   ();

    // ── Inspector panels (one per object type) ────────────────────────────────
    void DrawPanelPortal   (sqlite3*, bool placement);
    void DrawPanelTrigger  (sqlite3*, bool placement);
    void DrawPanelSoundZone(sqlite3*, bool placement);
    void DrawPanelColBox   (sqlite3*, bool placement);
    void DrawPanelColSphere(sqlite3*, bool placement);
    void DrawPanelWaypoint (sqlite3*, MediaTab*, bool placement);
    void DrawPanelNPC      (sqlite3*, MediaTab*, bool placement);
    void DrawPanelScenery  (sqlite3*, MediaTab*, bool placement);
    void DrawPanelTerrain  (sqlite3*, bool placement);
    void DrawPanelEmitters (sqlite3*, bool placement);
    void DrawPanelWater    (sqlite3*, bool placement);
    void DrawPanelEnviro      (sqlite3*);
    void DrawPanelOther       (sqlite3*);
    void DrawPanelSpawnPoint  (sqlite3*, MediaTab*, bool placement);

    // ── Undo ─────────────────────────────────────────────────────────────────
    enum UndoAction { kUndoCreate, kUndoDelete, kUndoMove, kUndoRotate, kUndoScale };
    struct UndoEntry {
        UndoAction action;
        int        objectType;
        int        objectId;
        glm::vec3  prevVec; // prev pos / rot / scale depending on action
    };
    static constexpr int kMaxUndo = 50;
    std::vector<UndoEntry> undoStack_;
    void PushUndo(UndoAction, int type, int id, glm::vec3 prevVec = {});
    void Undo(sqlite3*);

    // ── Actions ───────────────────────────────────────────────────────────────
    void PlaceObject      (const glm::vec3& worldPos, sqlite3*, MediaTab*);
    void DeleteSelected   (sqlite3*);
    void DuplicateSelected(sqlite3*);
    void FocusOnSelected  ();
    glm::vec3 RaycastScene(float vpX, float vpY);
    void SyncSceneryCache (MediaTab*);

    // ── Selection transform helpers (used by gizmo) ──────────────────────────
    bool SelectedPos       (glm::vec3& out) const;
    void SetSelectedPos    (const glm::vec3& pos);
    void PersistSelectedPos(sqlite3*);

    // Rotation (Euler degrees). Returns false for types that don't carry one.
    // NPCs return (0, yaw, 0); scenery returns its full rot. Others → false.
    bool SelectedRot       (glm::vec3& out) const;
    void SetSelectedRot    (const glm::vec3& rot);
    void PersistSelectedRot(sqlite3*);

    // Scale. Returns false when type has no scale (portal, trigger, …).
    // Water has only XZ scale (stored as .x and .z, y ignored).
    bool SelectedScale       (glm::vec3& out) const;
    void SetSelectedScale    (const glm::vec3& s);
    void PersistSelectedScale(sqlite3*);

    // ── Zone management ───────────────────────────────────────────────────────
    void FetchAreaList(sqlite3*);
    void LoadZone(sqlite3*, MediaTab*, const std::string& name);
    void SaveZone(sqlite3*);
    void EnsureScriptList();
    // Load media_materials rows into terrainMats_ for the terrain layer picker.
    void LoadTerrainMats(sqlite3* db);

    // ── Layout constants ──────────────────────────────────────────────────────
    static constexpr float kSidebarW   = 185.f;
    static constexpr float kInspectorW = 245.f;
    static constexpr float kStatusBarH =  24.f;

    // ── Core state ────────────────────────────────────────────────────────────
    ZoneCamera      cam_;
    ZoneRenderer    renderer_;
    ZoneScene       scene_;
    ThumbnailCache  thumbs_;

    ZoneMode     zoneMode_   = kModePortal;
    XFormMode    xformMode_  = kXFormSelect;

    int  selectedID_   = -1;
    int  selectedType_ = kSelNone;

    // Viewport interaction
    bool      vpHovered_   = false;
    bool      mouseLook_   = false;
    bool      rmbGesturePending_ = false;
    bool      rmbGestureDidFly_ = false;
    bool      rmbWasDown_ = false;
    ImVec2    rmbGestureStartPos_ = {0.f, 0.f};
    double    rmbGestureStartTime_ = 0.0;
    bool      mmbPan_      = false;
    bool      altOrbit_    = false;
    bool      altDolly_    = false;
    glm::vec3 orbitTarget_ = {};
    ImVec2 vpOrigin_   = {0, 0};
    ImVec2 vpSize_     = {800, 600};

    // Context menu pending placement position
    glm::vec3 pendingPlacePos_ = {};

    // ── Gizmo drag state (shared across Move / Rotate / Scale) ───────────────
    // `gizmoAxis_` is the active axis during a drag (-1 = idle). Drag type is
    // inferred from `xformMode_` so we don't track that twice.
    int       gizmoAxis_       = -1;   // -1 none, 0=X, 1=Y, 2=Z, 3=center (scale)
    glm::vec3 gizmoStartPos_   = {};   // object pos at drag start
    glm::vec3 gizmoStartRot_   = {};   // object rot at drag start (Euler deg)
    glm::vec3 gizmoStartScale_ = {1,1,1};
    float     gizmoStartS_     = 0.f;  // cursor param at drag start (axis-local)
    glm::vec3 gizmoPrePos_     = {};   // pre-drag snapshot for undo
    glm::vec3 gizmoPreRot_     = {};
    glm::vec3 gizmoPreScale_   = {1,1,1};

    // Zone list
    std::vector<std::string> areaList_;
    int    selectedArea_   = -1;
    bool   needFetchAreas_ = true;

    // Last MediaTab::MediaRevision() we synced against. Different = re-sync.
    int    last_media_revision_ = -1;
    char   newAreaBuf_[128] = {};
    bool   showNewArea_    = false;
    char   statusMsg_[256] = "Ready.";

    // ── Placement params ──────────────────────────────────────────────────────
    // Portal
    char  portalNameBuf_[64]     = {};
    char  portalLinkAreaBuf_[64] = {};
    char  portalLinkNameBuf_[64] = {};
    float portalRadius_          = 3.f;
    // Trigger
    char  trigScriptBuf_[128] = {};
    char  trigFuncBuf_[64]    = {};
    bool  trigOnce_           = false;
    float trigRadius_         = 5.f;
    // Sound zone
    char  sndNameBuf_[128] = {};
    int   sndVolume_       = 100;
    int   sndLoopMs_       = 0;
    float sndRadius_       = 15.f;
    // ColBox
    float cbScaleX_ = 5.f, cbScaleY_ = 2.f, cbScaleZ_ = 5.f;
    // ColSphere
    float csRadius_ = 3.f;
    // NPC
    int   npcActorDefId_       = 0;
    int   npcLastActorDefId_   = -1;  // detects change → auto-fill defaults
    char  npcNameBuf_[64] = {};
    char  npcRaceBuf_[64] = {};
    char  npcClassBuf_[64]= {};
    int   npcLevel_       = 1;
    int   npcAgg_         = 0;
    float npcAggroRange_  = 8.f;
    float npcAtkRange_    = 2.f;
    int   npcRespawnMs_   = 30000;
    char  npcSpawnScript_[128] = {}, npcSpawnFunc_[64] = {};
    char  npcClickScript_[128] = {}, npcClickFunc_[64] = {};
    char  npcDeathScript_[128] = {}, npcDeathFunc_[64] = {};
    // Waypoint link mode
    bool  wpLinkMode_  = false;
    bool  wpLinkB_     = false;
    // Water
    float wtrScaleX_ = 16.f, wtrScaleZ_ = 16.f;
    glm::vec3 wtrColor_ = {0.f, 0.39f, 0.59f};
    int   wtrOpacity_ = 50;
    int   wtrDamage_  = 0;
    // Scenery
    int   scnModelId_     = 0;
    int   scnMaterialId_  = 0;
    bool  scnAlignGround_ = true;   // default on — ground snap expected
    char  scnFilter_[128] = {};     // asset browser search filter
    bool  scnSnapGrid_    = false;  // G key toggles grid snap
    float scnGridSize_    = 1.0f;   // grid step in world units
    float scnRotSnap_     = 45.f;   // rotation snap in degrees (0 = free)
    // Terrain brush state
    int   brushMode_      = 0;     // 0=Raise 1=Lower 2=Smooth 3=Flatten 4=Paint
    int   brushFalloff_   = 0;     // 0=Smooth 1=Gaussian 2=Linear 3=Spherical
    float brushRadius_    = 10.f;
    float brushStrength_  = 1.0f;
    float brushFlattenH_  = 10.f;
    int   brushMaterial_  = 0;     // 0..3 — splatmap channel when painting
    bool  brushActive_    = false; // true while LMB is held in terrain mode

    // Brush cursor (terrain mode) — updated every frame via hover raycast
    glm::vec3 brushHitPos_     = {};
    bool      brushHoverValid_ = false;

    // Auto-paint slope state
    int   slopeFlatLayer_ = 0;
    int   slopeRockLayer_ = 2;
    float slopeMinDeg_    = 20.f;
    float slopeMaxDeg_    = 40.f;

    // Terrain undo/redo — one snapshot captured per brush stroke (not per frame)
    struct TerrainSnapshot {
        std::vector<float>   heights;
        std::vector<uint8_t> splat;
    };
    static constexpr int kMaxTerrainUndo = 30;
    std::vector<TerrainSnapshot> terrainUndo_;
    std::vector<TerrainSnapshot> terrainRedo_;
    bool terrainStrokeActive_ = false;

    // Emitters
    int   emtConfigIdx_   = 0;
    static constexpr const char* kEmitterNames[] = {
        "Fire", "Explosion", "Heal", "Portal", "Blood", "Smoke"
    };

    // Spawn Points
    float spawnPtRadius_      = 5.f;
    int   spawnPtSelMob_      = -1;   // index into selected spawn point's mobs

    std::vector<std::string>  scriptList_;
    bool                      scriptListLoaded_ = false;

    // Terrain material picker — populated from media_materials
    std::vector<TerrainMediaMat> terrainMats_;
    bool                         terrainMatsLoaded_ = false;

    // Mesh triangle cache for collision vis — model-local tris, keyed by model_id.
    MeshTriCache meshTriCache_;

    // Live-sync: poll zone_dirty table every ~1s; reload if version changed.
    int    zoneDirtyVersion_ = -1;  // last known version (-1 = not yet read)
    double zoneDirtyNextMs_  = 0.0; // next check time (chrono ms)
};

} // namespace gue
