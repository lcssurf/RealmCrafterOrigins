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
#include <unordered_set>

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
    kModeSpawnPoint  = 12,
    kModeColSphere   = 13,
    kModePlayerSpawn = 14,
    kModeFoliage     = 15,
    kModeLight       = 16,
    kModeCount       = 17,
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

enum GizmoSpaceMode {
    kGizmoSpaceWorld = 0,
    kGizmoSpaceLocal = 1,
};

enum GizmoPivotMode {
    kGizmoPivotOrigin = 0,
    kGizmoPivotBase   = 1,
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
    void DrawTopBar      (sqlite3* db, MediaTab* media);
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
    void DrawPanelWater    (sqlite3*, MediaTab*, bool placement);
    void DrawPanelEnviro      (sqlite3*);
    void DrawPanelOther       (sqlite3*);
    void DrawPanelSpawnPoint  (sqlite3*, MediaTab*, bool placement);
    void DrawPanelPlayerSpawn (sqlite3*, bool placement);
    void DrawPanelFoliage     (sqlite3*, MediaTab*, bool placement);
    void DrawPanelLight       (sqlite3*, bool placement);

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
    // Scatters (erase=false) or removes (erase=true) foliage instances within
    // foliageRadius_ of `hit`. Called every frame the LMB is held in Foliage
    // mode — reuses the zone_scenery table/ZScenery pipeline, so painted
    // instances are ordinary scenery (selectable, editable, deletable via the
    // normal Scenery panel/gizmo).
    void ApplyFoliageBrush(sqlite3* db, MediaTab* media, const glm::vec3& hit,
                           float dt, bool erase);
    void DeleteSelected   (sqlite3*);
    void DuplicateSelected(sqlite3*, MediaTab*);
    void CopySelected     ();
    void PasteSelected    (sqlite3*, MediaTab*);
    void FocusOnSelected  ();
    glm::vec3 RaycastScene(float vpX, float vpY);
    void SyncSceneryCache (MediaTab*);

    struct SelectionRef {
        int type = kSelNone;
        int id   = -1;
    };
    bool IsInSelection   (int type, int id) const;
    void ClearSelection  ();
    void SelectSingle    (int type, int id);
    void AddSelection    (int type, int id, bool makePrimary);
    void RemoveSelection (int type, int id);
    void ToggleSelection (int type, int id);
    std::vector<SelectionRef> ActiveSelection() const;

    // Shown instead of DrawPanelScenery when 2+ scenery are selected at once
    // (e.g. via clicking a folder header) — bulk "move to folder" + delete.
    void DrawPanelSceneryBulk(sqlite3*, MediaTab*, const std::vector<SelectionRef>& sel);

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

    // ── Scenery folders ───────────────────────────────────────────────────────
    // Registers `name` in zone_scenery_folders so it exists (and shows up in
    // the sidebar tree) even before any scenery is tagged with it. Safe to
    // call for an already-registered name (INSERT OR IGNORE, no-op).
    void CreateSceneryFolder(sqlite3* db, const std::string& name);
    // Deletes every scenery whose folder equals `folder` or is nested under it
    // ("folder/..."), plus the folder's own registry row(s). Confirmed via a
    // modal before calling (see zones_sidebar.cpp).
    void DeleteSceneryFolder(sqlite3* db, MediaTab* media, const std::string& folder);
    // Renames `folder` to `newFolder` for every scenery in it or nested under
    // it, preserving the nested suffix (e.g. "Forest/Undergrowth" renaming
    // "Forest"->"Woods" becomes "Woods/Undergrowth").
    void RenameSceneryFolder(sqlite3* db, MediaTab* media,
                             const std::string& folder, const std::string& newFolder);
    // Bulk-assigns `newFolder` to every scenery id in `ids` (used by the
    // "Move N selected -> folder" action when 2+ scenery are selected).
    void MoveSceneryToFolder(sqlite3* db, const std::vector<int>& ids, const std::string& newFolder);
    // Distinct folder names currently in use, sorted, for the picker combos.
    std::vector<std::string> DistinctSceneryFolders() const;

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
    std::vector<SelectionRef> selectedRefs_;
    std::vector<SelectionRef> selectionClipboard_;

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
    glm::vec3 gizmoStartObjPos_= {};   // object origin at drag start
    glm::vec3 gizmoStartHit_   = {};   // plane-drag anchor hit point
    glm::vec3 gizmoStartRot_   = {};   // object rot at drag start (Euler deg)
    glm::vec3 gizmoStartScale_ = {1,1,1};
    float     gizmoStartS_     = 0.f;  // cursor param at drag start (axis-local)
    float     gizmoRotAccumDeg_= 0.f;  // incremental rotate accumulator (avoids wrap jumps)
    float     gizmoLastAngle_  = 0.f;  // last ring angle sample (radians)
    glm::vec3 gizmoPrePos_     = {};   // pre-drag snapshot for undo
    glm::vec3 gizmoPreRot_     = {};
    glm::vec3 gizmoPreScale_   = {1,1,1};
    struct GizmoSelectionStart {
        int       type = kSelNone;
        int       id   = -1;
        bool      hasPos   = false;
        bool      hasRot   = false;
        bool      hasScale = false;
        glm::vec3 pos      = {};
        glm::vec3 rot      = {};
        glm::vec3 scale    = {1, 1, 1};
    };
    std::vector<GizmoSelectionStart> gizmoSelectionStart_;

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
    // Water texture: stored as ZWater::texPath (a media_materials.albedo_path
    // string, same convention as scenery's material picker), no dedicated
    // water texture table — reuses the existing Materials list from Media tab.
    std::string wtrTexPath_;
    float       wtrTexScale_ = 15.f;
    // Water Phase 1 — procedural wave normal. Direction is edited as an
    // angle (0-360°) in the UI (simpler than two raw X/Z sliders the dev
    // has to keep normalized) and converted to ZWater::waveDirX/waveDirZ
    // on write. Defaults match ZWater's own defaults (zone_scene.h).
    float wtrWaveSpeed_    = 0.3f;
    float wtrWaveAngleDeg_ = 45.f;   // atan2(0.7071, 0.7071) == 45°
    float wtrWaveScale_    = 0.35f;
    // Water Sub-fase 2a — depth-based transparency. Defaults match ZWater's
    // own defaults (zone_scene.h).
    glm::vec3 wtrShallowColor_      = {0.3f, 0.7f, 0.6f};
    glm::vec3 wtrDeepColor_         = {0.02f, 0.10f, 0.20f};
    float     wtrDepthFadeDistance_ = 2.5f;
    // Water Sub-fase 2b — procedural shoreline foam.
    float     wtrFoamWidth_ = 0.4f;
    glm::vec3 wtrFoamColor_ = {1.f, 1.f, 1.f};
    // Scenery
    int   scnModelId_     = 0;
    int   scnMaterialId_  = 0;
    bool  scnAlignGround_ = true;   // default on — ground snap expected
    char  scnFilter_[128] = {};     // asset browser search filter
    // Organizational folder new scenery placements are tagged with ("" =
    // ungrouped/root). Supports "/" nesting. See ZScenery::folder.
    char  scnFolder_[128] = {};

    // Scenery folder tree (scene sidebar) — delete/rename/create modal state.
    bool        showSceneryFolderDelete_ = false;
    std::string sceneryFolderDeleteTarget_;
    bool        showSceneryFolderRename_ = false;
    std::string sceneryFolderRenameTarget_;
    char        sceneryFolderRenameBuf_[128] = {};
    bool        showSceneryFolderCreate_ = false;
    char        sceneryFolderCreateBuf_[128] = {};
    // Shift+click range-select anchor for the sidebar Scenery tree (mirrors
    // the Models list's Ctrl/Shift picker in media.cpp).
    int         sceneryMultiSelAnchorId_ = -1;
    // Transient buffer backing the current drag-drop payload (folder move by
    // drag). Filled right before BeginDragDropSource; ImGui copies the bytes
    // immediately so this only needs to be valid for that one call.
    std::vector<int> sceneryDragPayload_;
    bool  scnSnapGrid_    = false;  // G key toggles grid snap
    float scnGridSize_    = 1.0f;   // grid step in world units
    float scnRotSnap_     = 45.f;   // rotation snap in degrees (0 = free)
    GizmoSpaceMode gizmoSpace_ = kGizmoSpaceWorld;
    GizmoPivotMode gizmoPivot_ = kGizmoPivotOrigin;
    bool  scnObjSnap_     = false;  // pivot-to-pivot alignment while moving gizmo
    float scnObjSnapDist_ = 1.0f;   // max axis distance to snap
    bool  scnFaceSnap_    = false;  // face-to-face surface snap while moving gizmo
    float scnFaceSnapDist_= 0.75f;  // max gap distance for face snap
    bool  scnAlignNormal_ = false;  // align yaw to snapped surface normal (XZ only)
    bool  scnAutoRotate_  = false;  // auto-rotate yaw on face snap to nearest snapped angle
    bool  gizmoMoveRotChanged_ = false;
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

    // Foliage brush state (scatter-paint trees/bushes/rocks as zone_scenery).
    // Candidate models are toggled on/off in the Asset Browser tiles while
    // this mode is active (see DrawTile in zones.cpp) instead of the single
    // scnModelId_ used by plain Scenery placement.
    std::vector<int> foliageModelIds_;
    char             foliageFilter_[128] = {};  // search filter for the in-panel model picker
    float foliageRadius_     = 8.f;    // paint circle radius, world units
    float foliageDensity_    = 0.35f;  // target instances per m² per second while held
    float foliageMinSpacing_ = 1.0f;   // reject a candidate closer than this to existing scenery
    float foliageMinScale_   = 0.85f;  // random per-instance scale jitter, multiplies the
    float foliageMaxScale_   = 1.25f;  // model's own default scale (media_models.scale)
    bool  foliageRandomYaw_  = true;
    bool  foliageActive_     = false;  // true while LMB held in Foliage mode
    float foliagePlaceAccum_ = 0.f;    // fractional-instance budget accumulator (frame-rate independent)
    // Organizational folder — new brush placements are tagged with this
    // (mirrors scnFolder_), and it also doubles as the erase mask target
    // in kFoliageEraseFolder mode (see ApplyFoliageBrush). "" = root.
    char  foliageFolder_[128]     = {};
    // Shift+LMB erase mask. Palette = only checked models; Folder = any
    // model whose folder matches foliageFolder_ (ignores palette); Any =
    // erase every scenery in the brush radius, no mask at all.
    enum FoliageEraseMode { kFoliageErasePalette = 0, kFoliageEraseFolder = 1, kFoliageEraseAny = 2 };
    int   foliageEraseMode_ = kFoliageErasePalette;
    // The first time a given model_id is ever scattered, SyncSceneryCache ->
    // Actor::Init pays a one-time cost (disk load + texture decode + GL
    // upload for that mesh's whole material set). Placing several instances
    // of a brand-new mesh in one tick would stack that cost inside a single
    // frame. ApplyFoliageBrush caps the per-tick budget to 1 as long as the
    // palette still has a model not yet in this set, then adds it once
    // placed — so cold loads are spread one-per-frame instead of bursting.
    std::unordered_set<int> foliageWarmModelIds_;

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

    // Point lights (Phase 1 — static torches/lanterns)
    char  lightName_[128]  = {};
    float lightColor_[3]   = {1.0f, 0.8f, 0.5f};  // warm torch-ish default
    float lightIntensity_  = 1.0f;
    float lightRadius_     = 5.0f;

    // Spawn Points
    float spawnPtRadius_      = 5.f;
    int   spawnPtSelMob_      = -1;   // index into selected spawn point's mobs
    bool  needsSpawnReload_   = false; // set when player_spawns table is mutated outside PlaceObject

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
