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
};

struct ZEmitter {
    int         id         = 0;
    glm::vec3   pos        = {};
    std::string configName;
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
    std::vector<ZWaypoint>   waypoints;
    std::vector<ZNpcSpawn>    npcs;
    std::vector<ZSpawnPoint>  spawnPoints;
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
