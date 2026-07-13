#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
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

struct ZEnvConfig {
    std::string name;
    int   musicTrack   = 1;
    float fogDensity   = 0.f;
    bool  isOutdoor    = true;
    bool  pvpEnabled   = false;
    float fogNear      = 300.f;
    float fogFar       = 600.f;
    float fogR         = 0.70f;
    float fogG         = 0.75f;
    float fogB         = 0.80f;
    int   ambientR     = 80;
    int   ambientG     = 80;
    int   ambientB     = 90;
    float gravity      = 1.0f;
    std::string entryScript, exitScript;
    int   weatherRain  = 0;
    int   weatherSnow  = 0;
    int   weatherFog   = 0;
    int   weatherStorm = 0;
    int   weatherWind  = 0;
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

    void Clear() {
        areaName.clear();
        scenery.clear(); portals.clear(); triggers.clear();
        soundZones.clear(); colBoxes.clear(); colSpheres.clear(); water.clear();
        emitters.clear(); waypoints.clear(); npcs.clear();
        spawnPoints.clear();
        playerSpawns.clear();
        lights.clear();
        sceneryFolders.clear();
        env = {};
        dirty = false;
        colVis = {};
        colVisDirty = true;
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

    // Schema migrations — run once per EnsureTables call (safe to call repeatedly).
    static void EnsureTables(sqlite3* db);
};

} // namespace gue
