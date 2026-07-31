#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <sqlite3.h>

namespace gue {

// ─── Zone object types ───────────────────────────────────────────────────────

struct ZScenery {
    int       id          = 0;
    int       modelId     = 0;
    int       materialId  = 0;
    glm::vec3 pos         = {};
    glm::vec3 rot         = {};   // pitch/yaw/roll degrees
    glm::vec3 scale       = {1,1,1};
    int       collision   = 1;    // 0=none 1=sphere 2=box/wedge 3=polygon
    int       animMode    = 0;    // 0=none 1=loop 2=ping-pong 3=on-select
    int       invSize     = 0;
    bool      ownable     = false;
    bool      locked      = false;
    // Excludes this object from the static coldata.bin bake — its collision
    // is instead recomputed every frame client-side from the current (script-
    // animated) pose. Only box/wedge collision shapes are supported for
    // dynamic objects (see the GUE panel warning) — mesh is not sent over
    // the network at all for dynamic objects, that's the expensive case the
    // dynamic-collision design deliberately avoids.
    bool      isDynamic   = false;
    // Gates the client's crosshair/reticle "[F] Interagir" prompt and the
    // PObjectInteract send — the client's world_static_objects hit-test has
    // no way to know whether a placed object actually has an
    // object_interact Lua handler bound to its id, so it must be told
    // explicitly. Defaults to false (opt-in): most scenery is decorative.
    bool      interactable = false;
    // Free-text organizational group ("" = ungrouped/root). Purely an
    // editor-side concept — not read by the server/client, just lets the
    // dev group placed scenery (e.g. "Forest North", "Village Props") for
    // bulk select/move/delete and Foliage Brush erase masking. Supports
    // "/" nesting the same way media asset names do.
    std::string folder;
};

struct ZPortal {
    int         id         = 0;
    glm::vec3   pos        = {};
    float       radius     = 3.f;
    std::string name;
    std::string linkArea;
    std::string linkPortal;
};

struct ZTrigger {
    int         id     = 0;
    float       x      = 0.f;
    float       z      = 0.f;
    float       radius = 5.f;
    std::string script;
    std::string func;
    bool        once   = false;
};

struct ZSoundZone {
    int         id        = 0;
    float       x         = 0.f;
    float       z         = 0.f;
    float       radius    = 15.f;
    std::string soundName;
    int         volume    = 100;
    int         loopMs    = 0;
};

struct ZColBox {
    int       id    = 0;
    glm::vec3 pos   = {};
    glm::vec3 scale = {5,2,5};
};

struct ZColSphere {
    int       id     = 0;
    glm::vec3 pos    = {};
    float     radius = 3.f;
};

struct ZWater {
    int         id        = 0;
    glm::vec3   pos       = {};
    glm::vec2   scale     = {16,16};
    glm::vec3   color     = {0.f, 0.39f, 0.59f};
    int         opacity   = 50;    // 0-100
    std::string texPath;
    float       texScale  = 15.f;
    int         damage    = 0;
    int         dmgType   = 0;
    // Gerstner wave params (water.vs — real vertex displacement, no normal
    // map file). waveDir is normalized in the shader; keep non-zero so
    // normalize() never sees a zero vector. waveScale is the primary wave's
    // angular wavenumber k (rad/world-unit) directly — wavelength=2*PI/k.
    // See zone_scene.cpp EnsureTables for the idempotent ADD COLUMN
    // migration and its default-value rationale.
    float       waveSpeed = 0.3f;
    float       waveDirX  = 0.7071f;
    float       waveDirZ  = 0.7071f;
    float       waveScale = 0.35f;
    // Sub-fase 2a — transparência por profundidade (water.fs samples
    // gDepth_ via Pipeline::SceneDepthTexture()). shallowColor/deepColor
    // replace the old flat `color` field's role in the lit shading (color
    // is still stored/loaded but no longer used for the water's hue — see
    // water.fs for the exact composition). depthFadeDistance is world
    // units of depth at which the gradient is ~63% (1-1/e) toward deepColor.
    glm::vec3   shallowColor      = {0.3f, 0.7f, 0.6f};
    glm::vec3   deepColor         = {0.02f, 0.10f, 0.20f};
    float       depthFadeDistance = 2.5f;
    // Sub-fase 2b — procedural shoreline foam. Reuses the same depthDiff as
    // shallowColor/deepColor (no separate depth logic) — foamWidth is how
    // far (world units) from a depthDiff=0 shoreline/submerged-object edge
    // the foam band extends.
    float       foamWidth = 0.4f;
    glm::vec3   foamColor = {1.f, 1.f, 1.f};
};

struct ZEmitter {
    int         id         = 0;
    glm::vec3   pos        = {};
    std::string configName;
    // fx_template_id — the real, working config path (server LoadZoneEmitters
    // only sends rows where this is >0; the legacy configName enum above is
    // GUE-preview-only, never reaches the server or game client). 0 = not
    // configured yet. loop: true = runs for as long as the area is loaded
    // (duration=-1 client-side), false = fires once and stops.
    int         fxTemplateId = 0;
    bool        loop         = true;
};

// Static point light (torch/lantern) — Phase 1 of the point-light system.
// Resubmitted every frame by the client's LightManager into
// Pipeline::AddPointLight(); purely additive to the deferred lighting pass
// (sun + IBL are untouched). See doc/TECH_DEBT.md for Phase 2 (dynamic
// skill/FX lights) and point-light shadows, neither of which are in scope here.
struct ZLight {
    int         id        = 0;
    glm::vec3   pos        = {};
    std::string name;
    glm::vec3   color      = {1.0f, 0.8f, 0.5f};  // warm torch-ish default
    float       intensity  = 1.0f;
    float       radius     = 5.0f;                // attenuation cutoff, world units

    // Phase 2 — additive. lightType 0=Point (default, everything below
    // unused, identical to Phase 1) 1=Spot 2=Directional. yaw/pitch: same
    // convention as scenery/NPC (no roll). coneAngle: Spot only, full cone
    // angle in degrees.
    int   lightType  = 0;
    float yaw        = 0.f;
    float pitch      = 0.f;
    float coneAngle  = 45.f;
};

struct ZWaypoint {
    int         id            = 0;
    glm::vec3   pos           = {};
    int         nextA         = -1;
    int         nextB         = -1;
    int         pauseSec      = 0;
    int         spawnActorId  = 0;
    std::string spawnScript,  spawnFunc;
    std::string clickScript,  clickFunc;
    std::string deathScript,  deathFunc;
    int         spawnDelaySec = 5;
    int         spawnMax      = 1;
    float       spawnRange    = 0.f;
};

struct ZNpcSpawn {
    int         id             = 0;
    int         actorDefId     = 0;
    std::string name;
    std::string race;
    std::string class_;
    int         level          = 1;
    glm::vec3   pos            = {};
    float       yaw            = 0.f;
    int         aggressiveness = 0;    // 0=passive 1=defensive 2=aggressive 3=dialog
    float       aggroRange     = 8.f;
    float       attackRange    = 2.f;
    int         respawnDelayMs = 30000;
    // Script hooks (stored in npc_spawns via ALTER TABLE)
    std::string spawnScript,  spawnFunc;
    std::string clickScript,  clickFunc;
    std::string deathScript,  deathFunc;
};

struct ZSpawnPointMob {
    int         id               = 0;
    int         actor_def_id     = 0;
    int         count            = 1;
    std::string name             = "NPC";
    std::string race             = "Human";
    std::string class_           = "Warrior";
    int         level            = 1;
    int         aggressiveness   = 2;
    float       aggressive_range = 8.f;
    float       attack_range     = 2.f;
    int         respawn_delay_ms = 30000;
};

struct ZSpawnPoint {
    int         id     = 0;
    std::string name;
    glm::vec3   pos    = {};
    float       radius = 5.f;
    std::vector<ZSpawnPointMob> mobs;
};

struct ZPlayerSpawn {
    int         id     = 0;
    std::string name;
    glm::vec3   pos    = {};
    float       yaw    = 0.f;
};

// Shared "authoritative render tuning" field set — exactly what AreaConfig
// sends the client via PAreaConfig (server/internal/net/client.go
// sendAreaConfig), MINUS skybox_hdr (a real HDR reload, not a cheap runtime
// value — see docs/TECH_DEBT.md "Atmosphere volumes" for the Fase 3 note)
// and MINUS terrain_* (a shader tiling knob, not atmosphere/mood). Used by
// BOTH ZEnvConfig (the area's own baseline) and ZAtmosphereVolume (a
// region's override) so ONE UI function (DrawAtmosphereFields, in
// zones_panels.cpp) can edit either — see docs/TECH_DEBT.md "Atmosphere
// volumes" for why this struct exists instead of two copies of the same 33
// fields. Defaults match sendAreaConfig's hardcoded fallback exactly.
struct ZAtmosphere {
    float sunDirX = 0.18f, sunDirY = 0.96f, sunDirZ = 0.20f;
    float sunColorR = 1.14f, sunColorG = 1.12f, sunColorB = 1.05f;
    float sunIntensityMul = 1.00f;
    float skyIntensityMul = 1.00f;
    float fogDensityMul   = 0.92f;
    float fogR = 0.70f, fogG = 0.80f, fogB = 0.93f;
    int   ambientR = 80, ambientG = 80, ambientB = 90;
    bool  volumetrics = true;

    float charShadowLift   = 0.30f;
    float charRimStrength  = 0.18f;
    float charRimExponent  = 2.40f;
    float charMinNdotL     = 0.10f;
    float charAmbientBoost = 0.12f;

    float sceneIblIntensity       = 1.00f;
    float sceneSkyIntensity       = 1.16f;
    float sceneWorldShadowLift    = 0.10f;
    float sceneDirectScale        = 1.32f;
    float sceneAmbientScale       = 0.88f;
    float sceneFlatAmbient        = 0.03f;
    float sceneWorldMinNdotL      = 0.05f;
    float sceneAlbedoMinLuma      = 0.18f;
    float sceneAlbedoLiftStrength = 0.00f;
    float sceneSpecularScale      = 0.88f;
    float sceneExposureFactor     = 1.10f;
    float sceneSunIntensity       = 1.36f;

    float colorContrast         = 1.08f;
    float colorSaturation       = 1.08f;
    float colorVibrance         = 0.20f;
    float colorBlackPoint       = 0.010f;
    float colorVignetteStrength = 0.04f;
    float colorVignetteSoftness = 0.55f;
};

struct ZEnvConfig {
    std::string name;
    int   musicTrack   = 1;
    float fogDensity   = 0.f;
    bool  isOutdoor    = true;
    bool  pvpEnabled   = false;
    float fogNear      = 300.f;
    float fogFar       = 600.f;
    // fogR/G/B and ambientR/G/B used to live here as separate top-level
    // fields (duplicating the same area_config.fog_r/g/b + ambient_r/g/b
    // columns also read into `atmo` below) — removed so there's a single
    // source of truth; use atmo.fogR/fogG/fogB and atmo.ambientR/G/B.
    // fogNear/fogFar/fogDensity above are a separate, older fog system
    // (distance-based) untouched by this change.
    float gravity      = 1.0f;
    std::string entryScript, exitScript;
    int   weatherRain  = 0;
    int   weatherSnow  = 0;
    int   weatherFog   = 0;
    int   weatherStorm = 0;
    int   weatherWind  = 0;

    // Authoritative render tuning (sun/scene-look/color-grading/character
    // readability) — previously only lived as area_config columns with no
    // GUE UI at all (only fog/ambient/weather/music/gravity above were
    // ever editable here). Added alongside ZAtmosphereVolume so the new
    // "Atmosphere Volume" panel and this area's own Environment tab can
    // share one drawing function instead of the volume inventing a second,
    // slightly-different set of controls. See docs/TECH_DEBT.md
    // "Atmosphere volumes".
    ZAtmosphere atmo;
};

// A placed region ("Post Process Volume" style) that overrides the area's
// atmosphere while the player is inside it — Fase 1 (this struct + editor)
// is data-only; the client-side point-in-volume test + enter/exit blend is
// Fase 2. Shape is either an AABB (pos ± size/2) or a sphere (radius =
// size.x/2) — the editor's generic bounding-box math (hit-test, gizmo
// pivot, face-snap) always uses the AABB form regardless of shape, same
// simplification already used for scenery's projected-corners hit-test; see
// docs/TECH_DEBT.md "Atmosphere volumes".
struct ZAtmosphereVolume {
    int         id       = 0;
    std::string name;
    int         shape    = 0;              // 0=AABB, 1=sphere
    glm::vec3   pos       = {};
    glm::vec3   size      = {10.f, 10.f, 10.f}; // AABB: full size. Sphere: size.x = diameter.
    int         priority  = 0;             // highest priority wins when volumes overlap
    float       transitionSeconds = 2.0f;  // fade in/out duration (Fase 2)
    ZAtmosphere atmo;
};

// ─── Collision shape visualisation (per-scenery, from media_model_shapes) ────
// Rebuilt by ZoneScene::RebuildColVis() and consumed by ZoneRenderer.
// All shapes are flattened into GL_LINES vertex data (3 floats per vertex,
// sequential pairs). Uploaded to a single GPU VBO; one draw call per frame.

struct ColVisData {
    // Interleaved: [pos.xyz  col.rgba] per vertex — GL_LINES pairs.
    // col is stored as 4 floats so boxes/spheres/tris can have distinct colours.
    struct Vtx { float x,y,z, r,g,b,a; };
    std::vector<Vtx> verts;  // always even count (pairs)
};

// Cache of extracted mesh triangles keyed by model_id (model-local space).
using MeshTriCache = std::unordered_map<int, std::vector<std::array<glm::vec3, 3>>>;

// ─── Full zone scene ─────────────────────────────────────────────────────────

struct ZoneScene {
    std::string              areaName;
    ZEnvConfig               env;
    std::vector<ZScenery>    scenery;
    std::vector<ZPortal>     portals;
    std::vector<ZTrigger>    triggers;
    std::vector<ZSoundZone>  soundZones;
    std::vector<ZColBox>     colBoxes;
    std::vector<ZColSphere>  colSpheres;
    std::vector<ZWater>      water;
    std::vector<ZEmitter>    emitters;
    std::vector<ZLight>      lights;
    std::vector<ZWaypoint>   waypoints;
    std::vector<ZNpcSpawn>    npcs;
    std::vector<ZSpawnPoint>  spawnPoints;
    std::vector<ZPlayerSpawn> playerSpawns;
    std::vector<ZAtmosphereVolume> atmosphereVolumes;
    // Registry of scenery organizational folders (zone_scenery_folders) —
    // tracked independently of ZScenery::folder so a folder can exist (and
    // show up in the scene sidebar) even with zero objects in it yet.
    // Folders that only exist as a tag on some ZScenery (not in this list,
    // e.g. legacy data) still work — the sidebar unions both sources.
    std::vector<std::string> sceneryFolders;
    bool dirty = false;

    // Pre-computed visualisation of per-scenery collision shapes.
    // Rebuilt on demand by RebuildColVis(); set colVisDirty=true to trigger.
    ColVisData colVis;
    bool       colVisDirty = true;

    // Separate wireframe preview for is_dynamic=1 scenery's box/wedge
    // collision shape (see RebuildDynamicColVis) — never baked into
    // coldata.bin and never merged into colVis's own storage; the two are
    // only combined at upload time (see zones_viewport.cpp), purely so the
    // existing single-batch GL draw call can render both without a second
    // VAO/VBO/shader.
    ColVisData dynColVis;

    // Global toggle for the combined colVis+dynColVis wireframe batch (see
    // ZoneRenderer::DrawForwardOverlays_) — on by default so existing
    // behavior is unchanged until a dev explicitly turns it off via the
    // "Colliders" button in the floating toolbar (zones_viewport.cpp
    // DrawFloatingToolbar). Editor-only, like hiddenObjects below — never
    // sent to client/server, never affects gameplay.
    bool showColliders = true;

    // ── Per-object editor visibility ("the eye icon") ─────────────────────
    // Generic across every selectable type (ZSelType enum) — NOT a `bool
    // visible` field duplicated on every ZScenery/ZLight/ZColBox/etc struct.
    // A (type,id) pair present in this set means "hidden"; absence means
    // visible (the common case, so nothing is written to the DB for
    // ordinary visible objects — see zone_object_visibility's schema in
    // zone_scene.cpp EnsureTables). Editor-only: never sent to the client/
    // server, never affects gameplay — see docs/TECH_DEBT.md
    // "Atmosphere volumes" (Fase 2 + visibility).
    std::unordered_set<int64_t> hiddenObjects;

    static int64_t VisKey(int type, int id) {
        return (static_cast<int64_t>(type) << 32) | static_cast<uint32_t>(id);
    }
    bool IsHidden(int type, int id) const {
        return hiddenObjects.count(VisKey(type, id)) != 0;
    }
    // Toggles hidden state and persists it: a hidden object gets one row
    // inserted into zone_object_visibility; making it visible again deletes
    // that row (so ordinary/visible objects never accumulate rows at all).
    void SetHidden(sqlite3* db, int type, int id, bool hidden);

    void Clear() {
        areaName.clear();
        scenery.clear(); portals.clear(); triggers.clear();
        soundZones.clear(); colBoxes.clear(); colSpheres.clear(); water.clear();
        emitters.clear(); waypoints.clear(); npcs.clear();
        spawnPoints.clear();
        playerSpawns.clear();
        lights.clear();
        atmosphereVolumes.clear();
        sceneryFolders.clear();
        hiddenObjects.clear();
        env = {};
        dirty = false;
        colVis = {};
        colVisDirty = true;
        dynColVis = {};
    }

    // Loads every object type for the given area from SQLite.
    // Called when the user changes zone; each phase adds its own table read.
    void LoadFromDB(sqlite3* db, const std::string& area);

    // Persists dirty state for all object types.
    void SaveToDB(sqlite3* db);

    // Writes dist/client/data/areas/<area>/coldata.bin — read by the client on area load.
    // Queries media_model_shapes for each Scenery with collision != None.
    void SaveColData(sqlite3* db, const std::string& area) const;

    // Rebuild per-scenery collision shape overlays for the viewport.
    // meshCache is keyed by model_id; triangles are extracted once and reused.
    void RebuildColVis(sqlite3* db, MeshTriCache& meshCache);

    // Same idea as RebuildColVis but for is_dynamic=1 scenery — a separate
    // wireframe (different color, see zone_scene.cpp) so it's visually
    // distinguishable in the editor. Only box/wedge shapes are drawn (the
    // only two types dynamic collision ever sends to the client — see
    // PDynamicCollisionShapes); this never touches colVis/coldata.bin.
    void RebuildDynamicColVis(sqlite3* db);

    // Schema migrations — run once per EnsureTables call (safe to call repeatedly).
    static void EnsureTables(sqlite3* db);
};

} // namespace gue
