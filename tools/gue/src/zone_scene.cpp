#include "zone_scene.h"
#include <rco/renderer/col_bake.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>

namespace gue {

// Helper — run a DDL statement, silently ignore "duplicate column" errors.
static void Exec(sqlite3* db, const char* sql) {
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

namespace {

struct QuantVtxKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

inline bool operator==(const QuantVtxKey& a, const QuantVtxKey& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

struct QuantVtxKeyLess {
    bool operator()(const QuantVtxKey& a, const QuantVtxKey& b) const {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    }
};

struct QuantVtxKeyHash {
    size_t operator()(const QuantVtxKey& v) const {
        size_t h = std::hash<int32_t>{}(v.x);
        h ^= std::hash<int32_t>{}(v.y) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(v.z) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct QuantTriKey {
    QuantVtxKey a;
    QuantVtxKey b;
    QuantVtxKey c;
};

inline bool operator==(const QuantTriKey& lhs, const QuantTriKey& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}

struct QuantTriKeyHash {
    size_t operator()(const QuantTriKey& t) const {
        QuantVtxKeyHash hv;
        size_t h = hv(t.a);
        h ^= hv(t.b) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= hv(t.c) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

size_t BuildQuantizedTriangles(
    const std::vector<std::array<glm::vec3, 3>>& src,
    int                                           gridDiv,
    float                                         areaCullFactor,
    std::vector<std::array<glm::vec3, 3>>*       out) {
    if (src.empty()) {
        if (out) out->clear();
        return 0;
    }

    gridDiv = std::max(1, gridDiv);

    glm::vec3 bmin = src[0][0];
    glm::vec3 bmax = src[0][0];
    for (const auto& tri : src) {
        for (const auto& p : tri) {
            bmin = glm::min(bmin, p);
            bmax = glm::max(bmax, p);
        }
    }
    glm::vec3 extent = bmax - bmin;
    float maxExtent = std::max(extent.x, std::max(extent.y, extent.z));
    if (maxExtent <= 1e-5f) {
        if (out) *out = src;
        return src.size();
    }

    float cell = maxExtent / (float)gridDiv;
    if (cell < 1e-6f) cell = 1e-6f;
    const float invCell = 1.f / cell;
    if (areaCullFactor < 1e-7f) areaCullFactor = 1e-7f;
    const float minArea2 = cell * cell * areaCullFactor;

    std::unordered_set<QuantTriKey, QuantTriKeyHash> unique;
    unique.reserve(src.size() * 2 + 1);

    std::vector<std::array<glm::vec3, 3>> tmp;
    if (out) tmp.reserve(src.size());

    auto quantize = [&](const glm::vec3& p, QuantVtxKey& q, glm::vec3& snapped) {
        q.x = (int32_t)std::lround((p.x - bmin.x) * invCell);
        q.y = (int32_t)std::lround((p.y - bmin.y) * invCell);
        q.z = (int32_t)std::lround((p.z - bmin.z) * invCell);
        snapped.x = bmin.x + (float)q.x * cell;
        snapped.y = bmin.y + (float)q.y * cell;
        snapped.z = bmin.z + (float)q.z * cell;
    };

    QuantVtxKeyLess lessKey;
    for (const auto& tri : src) {
        QuantVtxKey q[3];
        glm::vec3 s[3];
        quantize(tri[0], q[0], s[0]);
        quantize(tri[1], q[1], s[1]);
        quantize(tri[2], q[2], s[2]);

        if (q[0] == q[1] || q[1] == q[2] || q[2] == q[0]) continue;

        glm::vec3 cross = glm::cross(s[1] - s[0], s[2] - s[0]);
        if (glm::dot(cross, cross) <= minArea2) continue;

        std::array<QuantVtxKey, 3> sorted = {q[0], q[1], q[2]};
        std::sort(sorted.begin(), sorted.end(), lessKey);
        QuantTriKey key{sorted[0], sorted[1], sorted[2]};
        if (!unique.insert(key).second) continue;

        if (out) tmp.push_back({s[0], s[1], s[2]});
    }

    if (out) *out = std::move(tmp);
    return out ? out->size() : unique.size();
}

std::vector<std::array<glm::vec3, 3>> SimplifyTrianglesForBudget(
    const std::vector<std::array<glm::vec3, 3>>& src,
    float                                         budgetPct) {
    if (src.empty()) return {};

    budgetPct = std::clamp(budgetPct, 0.1f, 100.f);
    if (budgetPct >= 99.95f) return src;

    const float t = budgetPct / 100.f;
    const float curved = std::pow(t, 1.35f);
    const size_t target = std::max<size_t>(
        1, (size_t)std::lround(curved * (float)src.size()));
    if (target >= src.size()) return src;

    const float areaCullFactor = 1e-6f + (1.f - t) * 0.01f;

    auto absDiff = [](size_t a, size_t b) -> size_t { return a > b ? (a - b) : (b - a); };

    int lowDiv = 1;
    size_t lowCount = BuildQuantizedTriangles(src, lowDiv, areaCullFactor, nullptr);

    int highDiv = 2;
    size_t highCount = BuildQuantizedTriangles(src, highDiv, areaCullFactor, nullptr);
    while (highDiv < 1024 && highCount < target) {
        lowDiv = highDiv;
        lowCount = highCount;
        highDiv *= 2;
        highCount = BuildQuantizedTriangles(src, highDiv, areaCullFactor, nullptr);
    }

    int bestDiv = lowDiv;
    size_t bestCount = lowCount;
    auto consider = [&](int div, size_t count) {
        if (count == 0) return;
        size_t d = absDiff(count, target);
        size_t db = absDiff(bestCount, target);
        if (d < db || (d == db && count > bestCount)) {
            bestDiv = div;
            bestCount = count;
        }
    };

    consider(highDiv, highCount);
    int lo = lowDiv;
    int hi = highDiv;
    size_t loCount = lowCount;
    size_t hiCount = highCount;
    while (lo + 1 < hi) {
        const int mid = lo + (hi - lo) / 2;
        size_t midCount = BuildQuantizedTriangles(src, mid, areaCullFactor, nullptr);
        consider(mid, midCount);
        if (midCount < target) {
            lo = mid;
            loCount = midCount;
        } else {
            hi = mid;
            hiCount = midCount;
        }
    }
    consider(lo, loCount);
    consider(hi, hiCount);

    std::vector<std::array<glm::vec3, 3>> out;
    BuildQuantizedTriangles(src, bestDiv, areaCullFactor, &out);
    if (out.empty() && !src.empty()) out.push_back(src.front());
    return out;
}

} // namespace

void ZoneScene::EnsureTables(sqlite3* db) {
    // ── area_config expansions ────────────────────────────────────────────
    Exec(db, "ALTER TABLE area_config ADD COLUMN is_outdoor    INTEGER NOT NULL DEFAULT 1");
    Exec(db, "ALTER TABLE area_config ADD COLUMN pvp_enabled   INTEGER NOT NULL DEFAULT 0");
    Exec(db, "ALTER TABLE area_config ADD COLUMN fog_near      REAL    NOT NULL DEFAULT 300");
    Exec(db, "ALTER TABLE area_config ADD COLUMN fog_far       REAL    NOT NULL DEFAULT 600");
    Exec(db, "ALTER TABLE area_config ADD COLUMN fog_r         REAL    NOT NULL DEFAULT 0.7");
    Exec(db, "ALTER TABLE area_config ADD COLUMN fog_g         REAL    NOT NULL DEFAULT 0.75");
    Exec(db, "ALTER TABLE area_config ADD COLUMN fog_b         REAL    NOT NULL DEFAULT 0.8");
    Exec(db, "ALTER TABLE area_config ADD COLUMN ambient_r     INTEGER NOT NULL DEFAULT 80");
    Exec(db, "ALTER TABLE area_config ADD COLUMN ambient_g     INTEGER NOT NULL DEFAULT 80");
    Exec(db, "ALTER TABLE area_config ADD COLUMN ambient_b     INTEGER NOT NULL DEFAULT 90");
    Exec(db, "ALTER TABLE area_config ADD COLUMN gravity       REAL    NOT NULL DEFAULT 1.0");
    Exec(db, "ALTER TABLE area_config ADD COLUMN entry_script  TEXT    NOT NULL DEFAULT ''");
    Exec(db, "ALTER TABLE area_config ADD COLUMN exit_script   TEXT    NOT NULL DEFAULT ''");
    Exec(db, "ALTER TABLE area_config ADD COLUMN weather_rain  INTEGER NOT NULL DEFAULT 0");
    Exec(db, "ALTER TABLE area_config ADD COLUMN weather_snow  INTEGER NOT NULL DEFAULT 0");
    Exec(db, "ALTER TABLE area_config ADD COLUMN weather_fog   INTEGER NOT NULL DEFAULT 0");
    Exec(db, "ALTER TABLE area_config ADD COLUMN weather_storm INTEGER NOT NULL DEFAULT 0");
    Exec(db, "ALTER TABLE area_config ADD COLUMN weather_wind  INTEGER NOT NULL DEFAULT 0");

    // ── Spawn points ──────────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS spawn_points ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name      TEXT NOT NULL DEFAULT 'Spawn Point',"
        "  area_name TEXT NOT NULL DEFAULT '',"
        "  x         REAL NOT NULL DEFAULT 0,"
        "  y         REAL NOT NULL DEFAULT 0,"
        "  z         REAL NOT NULL DEFAULT 0,"
        "  radius    REAL NOT NULL DEFAULT 5"
        ")");
    Exec(db,
        "CREATE TABLE IF NOT EXISTS spawn_point_mobs ("
        "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  spawn_point_id   INTEGER NOT NULL DEFAULT 0,"
        "  actor_def_id     INTEGER NOT NULL DEFAULT 0,"
        "  mob_count        INTEGER NOT NULL DEFAULT 1,"
        "  name             TEXT NOT NULL DEFAULT 'NPC',"
        "  race             TEXT NOT NULL DEFAULT 'Human',"
        "  class            TEXT NOT NULL DEFAULT 'Warrior',"
        "  level            INTEGER NOT NULL DEFAULT 1,"
        "  aggressiveness   INTEGER NOT NULL DEFAULT 2,"
        "  aggressive_range REAL NOT NULL DEFAULT 8.0,"
        "  attack_range     REAL NOT NULL DEFAULT 2.0,"
        "  respawn_delay_ms INTEGER NOT NULL DEFAULT 30000"
        ")");

    // ── npc_spawns script columns ─────────────────────────────────────────
    Exec(db, "ALTER TABLE npc_spawns ADD COLUMN spawn_script TEXT NOT NULL DEFAULT ''");
    Exec(db, "ALTER TABLE npc_spawns ADD COLUMN spawn_func   TEXT NOT NULL DEFAULT ''");
    Exec(db, "ALTER TABLE npc_spawns ADD COLUMN click_script TEXT NOT NULL DEFAULT ''");
    Exec(db, "ALTER TABLE npc_spawns ADD COLUMN click_func   TEXT NOT NULL DEFAULT ''");
    Exec(db, "ALTER TABLE npc_spawns ADD COLUMN death_script TEXT NOT NULL DEFAULT ''");
    Exec(db, "ALTER TABLE npc_spawns ADD COLUMN death_func   TEXT NOT NULL DEFAULT ''");

    // ── Trigger zones ─────────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS area_triggers ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name    TEXT    NOT NULL DEFAULT '',"
        "  x            REAL    NOT NULL DEFAULT 0,"
        "  z            REAL    NOT NULL DEFAULT 0,"
        "  radius       REAL    NOT NULL DEFAULT 5,"
        "  script       TEXT    NOT NULL DEFAULT '',"
        "  func         TEXT    NOT NULL DEFAULT '',"
        "  trigger_once INTEGER NOT NULL DEFAULT 0"
        ")");

    // ── Sound zones ───────────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS area_sound_zones ("
        "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name        TEXT    NOT NULL DEFAULT '',"
        "  x                REAL    NOT NULL DEFAULT 0,"
        "  z                REAL    NOT NULL DEFAULT 0,"
        "  radius           REAL    NOT NULL DEFAULT 15,"
        "  sound_name       TEXT    NOT NULL DEFAULT '',"
        "  volume           INTEGER NOT NULL DEFAULT 100,"
        "  loop_interval_ms INTEGER NOT NULL DEFAULT 0"
        ")");

    // ── Collision boxes ───────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS zone_colboxes ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name TEXT    NOT NULL DEFAULT '',"
        "  x        REAL    NOT NULL DEFAULT 0,"
        "  y        REAL    NOT NULL DEFAULT 0,"
        "  z        REAL    NOT NULL DEFAULT 0,"
        "  scale_x  REAL    NOT NULL DEFAULT 5,"
        "  scale_y  REAL    NOT NULL DEFAULT 2,"
        "  scale_z  REAL    NOT NULL DEFAULT 5"
        ")");

    // ── Collision spheres ─────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS zone_colspheres ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name TEXT    NOT NULL DEFAULT '',"
        "  x         REAL    NOT NULL DEFAULT 0,"
        "  y         REAL    NOT NULL DEFAULT 0,"
        "  z         REAL    NOT NULL DEFAULT 0,"
        "  radius    REAL    NOT NULL DEFAULT 3"
        ")");

    // ── Waypoints ─────────────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS zone_waypoints ("
        "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name        TEXT    NOT NULL DEFAULT '',"
        "  x                REAL    NOT NULL DEFAULT 0,"
        "  y                REAL    NOT NULL DEFAULT 0,"
        "  z                REAL    NOT NULL DEFAULT 0,"
        "  next_a           INTEGER NOT NULL DEFAULT -1,"
        "  next_b           INTEGER NOT NULL DEFAULT -1,"
        "  pause_sec        INTEGER NOT NULL DEFAULT 0,"
        "  spawn_actor_id   INTEGER NOT NULL DEFAULT 0,"
        "  spawn_script     TEXT    NOT NULL DEFAULT '',"
        "  spawn_func       TEXT    NOT NULL DEFAULT '',"
        "  click_script     TEXT    NOT NULL DEFAULT '',"
        "  click_func       TEXT    NOT NULL DEFAULT '',"
        "  death_script     TEXT    NOT NULL DEFAULT '',"
        "  death_func       TEXT    NOT NULL DEFAULT '',"
        "  spawn_delay_sec  INTEGER NOT NULL DEFAULT 5,"
        "  spawn_max        INTEGER NOT NULL DEFAULT 1,"
        "  spawn_range      REAL    NOT NULL DEFAULT 0"
        ")");

    // ── Water ─────────────────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS zone_water ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name TEXT    NOT NULL DEFAULT '',"
        "  x        REAL    NOT NULL DEFAULT 0,"
        "  y        REAL    NOT NULL DEFAULT 0,"
        "  z        REAL    NOT NULL DEFAULT 0,"
        "  scale_x  REAL    NOT NULL DEFAULT 16,"
        "  scale_z  REAL    NOT NULL DEFAULT 16,"
        "  color_r  INTEGER NOT NULL DEFAULT 0,"
        "  color_g  INTEGER NOT NULL DEFAULT 100,"
        "  color_b  INTEGER NOT NULL DEFAULT 150,"
        "  opacity  INTEGER NOT NULL DEFAULT 50,"
        "  tex_path TEXT    NOT NULL DEFAULT '',"
        "  tex_scale REAL   NOT NULL DEFAULT 15,"
        "  damage   INTEGER NOT NULL DEFAULT 0,"
        "  dmg_type INTEGER NOT NULL DEFAULT 0"
        ")");
    // Water Phase 1 — procedural wave normal, tunable per-instance. Defaults
    // are non-zero on purpose: wave_speed=0/wave_scale=0 would freeze the
    // normal-perturbation math into a static (but still non-flat) look
    // instead of true stillness, and a zero wave_dir would normalize() to
    // NaN in the shader — these defaults keep existing (pre-Phase-1) water
    // rows visually reasonable without the dev having to touch them.
    Exec(db, "ALTER TABLE zone_water ADD COLUMN wave_speed REAL NOT NULL DEFAULT 0.3");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN wave_dir_x REAL NOT NULL DEFAULT 0.7071");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN wave_dir_z REAL NOT NULL DEFAULT 0.7071");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN wave_scale REAL NOT NULL DEFAULT 0.35");
    // Water Sub-fase 2a — depth-based transparency. Colors stored as 0-255
    // ints, same convention as color_r/g/b above. Defaults match ZWater's
    // own defaults (zone_scene.h) so pre-2a water rows get a sensible
    // shallow/deep gradient instead of shallowColor==deepColor==black.
    Exec(db, "ALTER TABLE zone_water ADD COLUMN shallow_r INTEGER NOT NULL DEFAULT 77");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN shallow_g INTEGER NOT NULL DEFAULT 178");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN shallow_b INTEGER NOT NULL DEFAULT 153");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN deep_r    INTEGER NOT NULL DEFAULT 5");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN deep_g    INTEGER NOT NULL DEFAULT 26");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN deep_b    INTEGER NOT NULL DEFAULT 51");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN depth_fade_distance REAL NOT NULL DEFAULT 2.5");
    // Water Sub-fase 2b — procedural shoreline foam. Reuses 2a's depth
    // comparison, no new depth logic. Defaults match ZWater's own defaults.
    Exec(db, "ALTER TABLE zone_water ADD COLUMN foam_width REAL    NOT NULL DEFAULT 0.4");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN foam_r     INTEGER NOT NULL DEFAULT 255");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN foam_g     INTEGER NOT NULL DEFAULT 255");
    Exec(db, "ALTER TABLE zone_water ADD COLUMN foam_b     INTEGER NOT NULL DEFAULT 255");

    // ── Emitters ──────────────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS zone_emitters ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name   TEXT    NOT NULL DEFAULT '',"
        "  config_name TEXT    NOT NULL DEFAULT '',"
        "  x           REAL    NOT NULL DEFAULT 0,"
        "  y           REAL    NOT NULL DEFAULT 0,"
        "  z           REAL    NOT NULL DEFAULT 0"
        ")");

    // ── Point lights (Phase 1 — static torches/lanterns) ───────────────────
    // Sent to the client on area load; resubmitted every frame into the
    // already-existing (previously unused) Pipeline::AddPointLight() deferred
    // light-volume pass. See server/internal/db/db.go LoadZoneLights and
    // client rco::renderer::LightManager.
    Exec(db,
        "CREATE TABLE IF NOT EXISTS zone_lights ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name TEXT    NOT NULL DEFAULT '',"
        "  name      TEXT    NOT NULL DEFAULT '',"
        "  x         REAL    NOT NULL DEFAULT 0,"
        "  y         REAL    NOT NULL DEFAULT 0,"
        "  z         REAL    NOT NULL DEFAULT 0,"
        "  color_r   REAL    NOT NULL DEFAULT 1.0,"
        "  color_g   REAL    NOT NULL DEFAULT 0.8,"
        "  color_b   REAL    NOT NULL DEFAULT 0.5,"
        "  intensity REAL    NOT NULL DEFAULT 1.0,"
        "  radius    REAL    NOT NULL DEFAULT 5.0"
        ")");

    // ── Player spawn points ───────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS player_spawns ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name      TEXT    NOT NULL DEFAULT '',"
        "  area_name TEXT    NOT NULL DEFAULT '',"
        "  x         REAL    NOT NULL DEFAULT 0,"
        "  y         REAL    NOT NULL DEFAULT 0,"
        "  z         REAL    NOT NULL DEFAULT 0,"
        "  yaw       REAL    NOT NULL DEFAULT 0"
        ")");

    // ── Scenery (props) ───────────────────────────────────────────────────
    Exec(db,
        "CREATE TABLE IF NOT EXISTS zone_scenery ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name    TEXT    NOT NULL DEFAULT '',"
        "  model_id     INTEGER NOT NULL DEFAULT 0,"
        "  material_id  INTEGER NOT NULL DEFAULT 0,"
        "  x            REAL    NOT NULL DEFAULT 0,"
        "  y            REAL    NOT NULL DEFAULT 0,"
        "  z            REAL    NOT NULL DEFAULT 0,"
        "  pitch        REAL    NOT NULL DEFAULT 0,"
        "  yaw          REAL    NOT NULL DEFAULT 0,"
        "  roll         REAL    NOT NULL DEFAULT 0,"
        "  sx           REAL    NOT NULL DEFAULT 1,"
        "  sy           REAL    NOT NULL DEFAULT 1,"
        "  sz           REAL    NOT NULL DEFAULT 1,"
        "  collision    INTEGER NOT NULL DEFAULT 1,"
        "  anim_mode    INTEGER NOT NULL DEFAULT 0,"
        "  inv_size     INTEGER NOT NULL DEFAULT 0,"
        "  ownable      INTEGER NOT NULL DEFAULT 0,"
        "  locked       INTEGER NOT NULL DEFAULT 0,"
        "  folder       TEXT    NOT NULL DEFAULT ''"
        ")");

    // media_model_shapes extra LOD params (safe no-op if missing/already exists).
    Exec(db, "ALTER TABLE media_model_shapes ADD COLUMN detail_a REAL NOT NULL DEFAULT 0");
    Exec(db, "ALTER TABLE media_model_shapes ADD COLUMN detail_b REAL NOT NULL DEFAULT 0");

    // Scenery organizational folders (safe no-op on DBs that already have it).
    Exec(db, "ALTER TABLE zone_scenery ADD COLUMN folder TEXT NOT NULL DEFAULT ''");

    // Folder registry — tracks a folder's existence independently of whether
    // any scenery is currently tagged with it, so "New Folder" always leaves
    // something visible in the sidebar even with zero contents.
    Exec(db,
        "CREATE TABLE IF NOT EXISTS zone_scenery_folders ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name TEXT NOT NULL DEFAULT '',"
        "  name      TEXT NOT NULL DEFAULT '',"
        "  UNIQUE(area_name, name)"
        ")");
}

// ─── LoadFromDB ──────────────────────────────────────────────────────────────

void ZoneScene::LoadFromDB(sqlite3* db, const std::string& area) {
    Clear();
    areaName = area;
    EnsureTables(db);

    sqlite3_stmt* stmt = nullptr;

    // ── Environment config ────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT music_track, fog_density, is_outdoor, pvp_enabled,"
        "       fog_near, fog_far, fog_r, fog_g, fog_b,"
        "       ambient_r, ambient_g, ambient_b, gravity,"
        "       entry_script, exit_script,"
        "       weather_rain, weather_snow, weather_fog, weather_storm, weather_wind"
        " FROM area_config WHERE name=?",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            env.name        = area;
            env.musicTrack  = sqlite3_column_int(stmt, 0);
            env.fogDensity  = (float)sqlite3_column_double(stmt, 1);
            env.isOutdoor   = sqlite3_column_int(stmt, 2) != 0;
            env.pvpEnabled  = sqlite3_column_int(stmt, 3) != 0;
            env.fogNear     = (float)sqlite3_column_double(stmt, 4);
            env.fogFar      = (float)sqlite3_column_double(stmt, 5);
            env.fogR        = (float)sqlite3_column_double(stmt, 6);
            env.fogG        = (float)sqlite3_column_double(stmt, 7);
            env.fogB        = (float)sqlite3_column_double(stmt, 8);
            env.ambientR    = sqlite3_column_int(stmt, 9);
            env.ambientG    = sqlite3_column_int(stmt, 10);
            env.ambientB    = sqlite3_column_int(stmt, 11);
            env.gravity     = (float)sqlite3_column_double(stmt, 12);
            auto txt = [&](int col) -> std::string {
                const char* t = (const char*)sqlite3_column_text(stmt, col);
                return t ? t : "";
            };
            env.entryScript  = txt(13);
            env.exitScript   = txt(14);
            env.weatherRain  = sqlite3_column_int(stmt, 15);
            env.weatherSnow  = sqlite3_column_int(stmt, 16);
            env.weatherFog   = sqlite3_column_int(stmt, 17);
            env.weatherStorm = sqlite3_column_int(stmt, 18);
            env.weatherWind  = sqlite3_column_int(stmt, 19);
        }
        sqlite3_finalize(stmt);
    }

    auto txt = [](sqlite3_stmt* s, int col) -> std::string {
        const char* t = (const char*)sqlite3_column_text(s, col);
        return t ? t : "";
    };

    // ── Portals ───────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, z, radius, target_area, dest_x, dest_y, dest_z, dest_yaw"
        " FROM area_portals WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZPortal p;
            p.id       = sqlite3_column_int(stmt, 0);
            p.pos.x    = (float)sqlite3_column_double(stmt, 1);
            p.pos.y    = (float)sqlite3_column_double(stmt, 7); // dest_y used as visual Y
            p.pos.z    = (float)sqlite3_column_double(stmt, 2);
            p.radius   = (float)sqlite3_column_double(stmt, 3);
            p.linkArea = txt(stmt, 4);
            // dest coords stored separately; for now pos.y=0 (portals are XZ)
            p.pos.y    = 0.f;
            portals.push_back(p);
        }
        sqlite3_finalize(stmt);
    }

    // ── Triggers ─────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, z, radius, script, func, trigger_once"
        " FROM area_triggers WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZTrigger t;
            t.id     = sqlite3_column_int(stmt, 0);
            t.x      = (float)sqlite3_column_double(stmt, 1);
            t.z      = (float)sqlite3_column_double(stmt, 2);
            t.radius = (float)sqlite3_column_double(stmt, 3);
            t.script = txt(stmt, 4);
            t.func   = txt(stmt, 5);
            t.once   = sqlite3_column_int(stmt, 6) != 0;
            triggers.push_back(t);
        }
        sqlite3_finalize(stmt);
    }

    // ── Sound zones ───────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, z, radius, sound_name, volume, loop_interval_ms"
        " FROM area_sound_zones WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZSoundZone s;
            s.id        = sqlite3_column_int(stmt, 0);
            s.x         = (float)sqlite3_column_double(stmt, 1);
            s.z         = (float)sqlite3_column_double(stmt, 2);
            s.radius    = (float)sqlite3_column_double(stmt, 3);
            s.soundName = txt(stmt, 4);
            s.volume    = sqlite3_column_int(stmt, 5);
            s.loopMs    = sqlite3_column_int(stmt, 6);
            soundZones.push_back(s);
        }
        sqlite3_finalize(stmt);
    }

    // ── Collision boxes ───────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, y, z, scale_x, scale_y, scale_z"
        " FROM zone_colboxes WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZColBox c;
            c.id       = sqlite3_column_int(stmt, 0);
            c.pos.x    = (float)sqlite3_column_double(stmt, 1);
            c.pos.y    = (float)sqlite3_column_double(stmt, 2);
            c.pos.z    = (float)sqlite3_column_double(stmt, 3);
            c.scale.x  = (float)sqlite3_column_double(stmt, 4);
            c.scale.y  = (float)sqlite3_column_double(stmt, 5);
            c.scale.z  = (float)sqlite3_column_double(stmt, 6);
            colBoxes.push_back(c);
        }
        sqlite3_finalize(stmt);
    }

    // ── Collision spheres ─────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, y, z, radius FROM zone_colspheres WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZColSphere s;
            s.id     = sqlite3_column_int(stmt, 0);
            s.pos.x  = (float)sqlite3_column_double(stmt, 1);
            s.pos.y  = (float)sqlite3_column_double(stmt, 2);
            s.pos.z  = (float)sqlite3_column_double(stmt, 3);
            s.radius = (float)sqlite3_column_double(stmt, 4);
            colSpheres.push_back(s);
        }
        sqlite3_finalize(stmt);
    }

    // ── Waypoints ─────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, y, z, next_a, next_b, pause_sec,"
        "       spawn_actor_id, spawn_script, spawn_func,"
        "       click_script, click_func, death_script, death_func,"
        "       spawn_delay_sec, spawn_max, spawn_range"
        " FROM zone_waypoints WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZWaypoint w;
            w.id            = sqlite3_column_int(stmt, 0);
            w.pos.x         = (float)sqlite3_column_double(stmt, 1);
            w.pos.y         = (float)sqlite3_column_double(stmt, 2);
            w.pos.z         = (float)sqlite3_column_double(stmt, 3);
            w.nextA         = sqlite3_column_int(stmt, 4);
            w.nextB         = sqlite3_column_int(stmt, 5);
            w.pauseSec      = sqlite3_column_int(stmt, 6);
            w.spawnActorId  = sqlite3_column_int(stmt, 7);
            w.spawnScript   = txt(stmt, 8);  w.spawnFunc  = txt(stmt, 9);
            w.clickScript   = txt(stmt,10);  w.clickFunc  = txt(stmt,11);
            w.deathScript   = txt(stmt,12);  w.deathFunc  = txt(stmt,13);
            w.spawnDelaySec = sqlite3_column_int(stmt, 14);
            w.spawnMax      = sqlite3_column_int(stmt, 15);
            w.spawnRange    = (float)sqlite3_column_double(stmt, 16);
            waypoints.push_back(w);
        }
        sqlite3_finalize(stmt);
    }

    // ── NPCs ─────────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, race, class, level, x, y, z, yaw,"
        "       aggressiveness, aggressive_range, attack_range, respawn_delay_ms,"
        "       actor_def_id,"
        "       spawn_script, spawn_func, click_script, click_func,"
        "       death_script, death_func"
        " FROM npc_spawns WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZNpcSpawn n;
            n.id             = sqlite3_column_int(stmt, 0);
            n.name           = txt(stmt, 1);
            n.race           = txt(stmt, 2);
            n.class_         = txt(stmt, 3);
            n.level          = sqlite3_column_int(stmt, 4);
            n.pos.x          = (float)sqlite3_column_double(stmt, 5);
            n.pos.y          = (float)sqlite3_column_double(stmt, 6);
            n.pos.z          = (float)sqlite3_column_double(stmt, 7);
            n.yaw            = (float)sqlite3_column_double(stmt, 8);
            n.aggressiveness = sqlite3_column_int(stmt, 9);
            n.aggroRange     = (float)sqlite3_column_double(stmt, 10);
            n.attackRange    = (float)sqlite3_column_double(stmt, 11);
            n.respawnDelayMs = sqlite3_column_int(stmt, 12);
            n.actorDefId     = sqlite3_column_int(stmt, 13);
            n.spawnScript    = txt(stmt,14); n.spawnFunc  = txt(stmt,15);
            n.clickScript    = txt(stmt,16); n.clickFunc  = txt(stmt,17);
            n.deathScript    = txt(stmt,18); n.deathFunc  = txt(stmt,19);
            npcs.push_back(n);
        }
        sqlite3_finalize(stmt);
    }

    // ── Emitters ─────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, y, z, config_name FROM zone_emitters WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZEmitter e;
            e.id         = sqlite3_column_int(stmt, 0);
            e.pos.x      = (float)sqlite3_column_double(stmt, 1);
            e.pos.y      = (float)sqlite3_column_double(stmt, 2);
            e.pos.z      = (float)sqlite3_column_double(stmt, 3);
            e.configName = txt(stmt, 4);
            emitters.push_back(e);
        }
        sqlite3_finalize(stmt);
    }

    // ── Point lights ─────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, y, z, name, color_r, color_g, color_b, intensity, radius"
        " FROM zone_lights WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZLight l;
            l.id        = sqlite3_column_int(stmt, 0);
            l.pos.x     = (float)sqlite3_column_double(stmt, 1);
            l.pos.y     = (float)sqlite3_column_double(stmt, 2);
            l.pos.z     = (float)sqlite3_column_double(stmt, 3);
            l.name      = txt(stmt, 4);
            l.color.r   = (float)sqlite3_column_double(stmt, 5);
            l.color.g   = (float)sqlite3_column_double(stmt, 6);
            l.color.b   = (float)sqlite3_column_double(stmt, 7);
            l.intensity = (float)sqlite3_column_double(stmt, 8);
            l.radius    = (float)sqlite3_column_double(stmt, 9);
            lights.push_back(l);
        }
        sqlite3_finalize(stmt);
    }

    // ── Water ─────────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, y, z, scale_x, scale_z,"
        "       color_r, color_g, color_b, opacity,"
        "       tex_path, tex_scale, damage, dmg_type,"
        "       wave_speed, wave_dir_x, wave_dir_z, wave_scale,"
        "       shallow_r, shallow_g, shallow_b, deep_r, deep_g, deep_b, depth_fade_distance,"
        "       foam_width, foam_r, foam_g, foam_b"
        " FROM zone_water WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZWater w;
            w.id        = sqlite3_column_int(stmt, 0);
            w.pos.x     = (float)sqlite3_column_double(stmt, 1);
            w.pos.y     = (float)sqlite3_column_double(stmt, 2);
            w.pos.z     = (float)sqlite3_column_double(stmt, 3);
            w.scale.x   = (float)sqlite3_column_double(stmt, 4);
            w.scale.y   = (float)sqlite3_column_double(stmt, 5);
            w.color.r   = (float)sqlite3_column_int(stmt, 6) / 255.f;
            w.color.g   = (float)sqlite3_column_int(stmt, 7) / 255.f;
            w.color.b   = (float)sqlite3_column_int(stmt, 8) / 255.f;
            w.opacity   = sqlite3_column_int(stmt, 9);
            w.texPath   = txt(stmt, 10);
            w.texScale  = (float)sqlite3_column_double(stmt, 11);
            w.damage    = sqlite3_column_int(stmt, 12);
            w.dmgType   = sqlite3_column_int(stmt, 13);
            w.waveSpeed = (float)sqlite3_column_double(stmt, 14);
            w.waveDirX  = (float)sqlite3_column_double(stmt, 15);
            w.waveDirZ  = (float)sqlite3_column_double(stmt, 16);
            w.waveScale = (float)sqlite3_column_double(stmt, 17);
            w.shallowColor.r      = (float)sqlite3_column_int(stmt, 18) / 255.f;
            w.shallowColor.g      = (float)sqlite3_column_int(stmt, 19) / 255.f;
            w.shallowColor.b      = (float)sqlite3_column_int(stmt, 20) / 255.f;
            w.deepColor.r         = (float)sqlite3_column_int(stmt, 21) / 255.f;
            w.deepColor.g         = (float)sqlite3_column_int(stmt, 22) / 255.f;
            w.deepColor.b         = (float)sqlite3_column_int(stmt, 23) / 255.f;
            w.depthFadeDistance   = (float)sqlite3_column_double(stmt, 24);
            w.foamWidth           = (float)sqlite3_column_double(stmt, 25);
            w.foamColor.r         = (float)sqlite3_column_int(stmt, 26) / 255.f;
            w.foamColor.g         = (float)sqlite3_column_int(stmt, 27) / 255.f;
            w.foamColor.b         = (float)sqlite3_column_int(stmt, 28) / 255.f;
            water.push_back(w);
        }
        sqlite3_finalize(stmt);
    }

    // ── Scenery ───────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, model_id, material_id,"
        "       x, y, z, pitch, yaw, roll, sx, sy, sz,"
        "       collision, anim_mode, inv_size, ownable, locked, folder"
        " FROM zone_scenery WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZScenery s;
            s.id         = sqlite3_column_int(stmt, 0);
            s.modelId    = sqlite3_column_int(stmt, 1);
            s.materialId = sqlite3_column_int(stmt, 2);
            s.pos        = {(float)sqlite3_column_double(stmt,3),
                            (float)sqlite3_column_double(stmt,4),
                            (float)sqlite3_column_double(stmt,5)};
            s.rot        = {(float)sqlite3_column_double(stmt,6),
                            (float)sqlite3_column_double(stmt,7),
                            (float)sqlite3_column_double(stmt,8)};
            s.scale      = {(float)sqlite3_column_double(stmt,9),
                            (float)sqlite3_column_double(stmt,10),
                            (float)sqlite3_column_double(stmt,11)};
            s.collision  = sqlite3_column_int(stmt, 12);
            s.animMode   = sqlite3_column_int(stmt, 13);
            s.invSize    = sqlite3_column_int(stmt, 14);
            s.ownable    = sqlite3_column_int(stmt, 15) != 0;
            s.locked     = sqlite3_column_int(stmt, 16) != 0;
            {
                const unsigned char* f = sqlite3_column_text(stmt, 17);
                s.folder = f ? reinterpret_cast<const char*>(f) : "";
            }
            scenery.push_back(s);
        }
        sqlite3_finalize(stmt);
    }

    // ── Scenery folder registry ─────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT name FROM zone_scenery_folders WHERE area_name=? ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* n = sqlite3_column_text(stmt, 0);
            if (n) sceneryFolders.push_back(reinterpret_cast<const char*>(n));
        }
        sqlite3_finalize(stmt);
    }

    // ── Spawn points ──────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, x, y, z, radius FROM spawn_points WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZSpawnPoint sp;
            sp.id     = sqlite3_column_int(stmt, 0);
            sp.name   = txt(stmt, 1);
            sp.pos.x  = (float)sqlite3_column_double(stmt, 2);
            sp.pos.y  = (float)sqlite3_column_double(stmt, 3);
            sp.pos.z  = (float)sqlite3_column_double(stmt, 4);
            sp.radius = (float)sqlite3_column_double(stmt, 5);
            spawnPoints.push_back(sp);
        }
        sqlite3_finalize(stmt);
    }
    // ── Player spawn points ───────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, x, y, z, yaw FROM player_spawns WHERE area_name=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZPlayerSpawn ps;
            ps.id    = sqlite3_column_int(stmt, 0);
            ps.name  = txt(stmt, 1);
            ps.pos.x = (float)sqlite3_column_double(stmt, 2);
            ps.pos.y = (float)sqlite3_column_double(stmt, 3);
            ps.pos.z = (float)sqlite3_column_double(stmt, 4);
            ps.yaw   = (float)sqlite3_column_double(stmt, 5);
            playerSpawns.push_back(ps);
        }
        sqlite3_finalize(stmt);
    }

    // Load mobs for each spawn point
    for (auto& sp : spawnPoints) {
        if (sqlite3_prepare_v2(db,
            "SELECT id, actor_def_id, mob_count, name, race, class,"
            " level, aggressiveness, aggressive_range, attack_range, respawn_delay_ms"
            " FROM spawn_point_mobs WHERE spawn_point_id=? ORDER BY id",
            -1, &stmt, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_int(stmt, 1, sp.id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ZSpawnPointMob m;
            m.id             = sqlite3_column_int(stmt, 0);
            m.actor_def_id   = sqlite3_column_int(stmt, 1);
            m.count          = sqlite3_column_int(stmt, 2);
            m.name           = txt(stmt, 3);
            m.race           = txt(stmt, 4);
            m.class_         = txt(stmt, 5);
            m.level          = sqlite3_column_int(stmt, 6);
            m.aggressiveness = sqlite3_column_int(stmt, 7);
            m.aggressive_range = (float)sqlite3_column_double(stmt, 8);
            m.attack_range     = (float)sqlite3_column_double(stmt, 9);
            m.respawn_delay_ms = sqlite3_column_int(stmt, 10);
            sp.mobs.push_back(m);
        }
        sqlite3_finalize(stmt);
    }
}

// ─── SaveToDB ────────────────────────────────────────────────────────────────
// Implemented incrementally — each phase adds its section.

void ZoneScene::SaveToDB(sqlite3* db) {
    (void)db;  // phases fill this in
    dirty = false;
}

// ─── SaveColData ──────────────────────────────────────────────────────────────
// Binary format (coldata.bin):
//   u32 magic = 0x444C4F43 ('COLD')
//   u32 version = 2
//   u32 num_boxes    — each: f32 cx,cy,cz, hx,hy,hz
//   u32 num_spheres  — each: f32 cx,cy,cz, r
//   u32 num_tris     — each: 9 f32 (v0.xyz, v1.xyz, v2.xyz) — world-space

void ZoneScene::SaveColData(sqlite3* db, const std::string& area) const {
    char path[512];
    std::snprintf(path, sizeof(path),
        "../client/data/areas/%s/coldata.bin", area.c_str());

    FILE* f = std::fopen(path, "wb");
    if (!f) return;

    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto wf  = [&](float    v) { std::fwrite(&v, 4, 1, f); };

    struct FlatBox    { float cx,cy,cz, hx,hy,hz; };
    struct FlatSphere { float cx,cy,cz, r; };
    struct FlatTri    { float ax,ay,az, bx,by,bz, cx_,cy_,cz_; };
    std::vector<FlatBox>    boxes;
    std::vector<FlatSphere> spheres;
    std::vector<FlatTri>    tris;

    // Standalone ColBoxes
    for (auto& b : colBoxes)
        boxes.push_back({b.pos.x, b.pos.y, b.pos.z,
                         b.scale.x*0.5f, b.scale.y*0.5f, b.scale.z*0.5f});

    // Standalone ColSpheres
    for (auto& s : colSpheres)
        spheres.push_back({s.pos.x, s.pos.y, s.pos.z, s.radius});

    // Scenery objects with collision != None (0):
    // look up their media_model_shapes, transform by instance TRS.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT type, offset_x, offset_y, offset_z, size_x, size_y, size_z, detail_a, detail_b"
        " FROM media_model_shapes WHERE model_id=? ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {

        sqlite3_stmt* pathStmt = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT file_path FROM media_models WHERE id=?",
            -1, &pathStmt, nullptr);

        auto appendWedgeTris = [&](float ox, float oy, float oz,
                                   float sx, float sy, float sz, int subdiv,
                                   const glm::mat4& trsMat) {
            const float hx = std::abs(sx) * 0.5f;
            const float hy = std::abs(sy) * 0.5f;
            const float hz = std::abs(sz) * 0.5f;
            const bool risePositiveX = sx >= 0.f;
            const float xLow  = risePositiveX ? (ox - hx) : (ox + hx);
            const float xHigh = risePositiveX ? (ox + hx) : (ox - hx);
            glm::vec3 v[6] = {
                {xLow,  oy - hy, oz - hz}, // low-back  bottom
                {xLow,  oy - hy, oz + hz}, // low-front bottom
                {xHigh, oy - hy, oz - hz}, // high-back bottom
                {xHigh, oy + hy, oz - hz}, // high-back top
                {xHigh, oy + hy, oz + hz}, // high-front top
                {xHigh, oy - hy, oz + hz}, // high-front bottom
            };
            auto pushTri = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, int n) {
                if (n < 1) n = 1;
                for (int i = 0; i < n; ++i) {
                    for (int j = 0; j < n - i; ++j) {
                        float fi0 = (float)i / (float)n;
                        float fj0 = (float)j / (float)n;
                        float fi1 = (float)(i + 1) / (float)n;
                        float fj1 = (float)(j + 1) / (float)n;
                        glm::vec3 p0 = a + (b - a) * fi0 + (c - a) * fj0;
                        glm::vec3 p1 = a + (b - a) * fi1 + (c - a) * fj0;
                        glm::vec3 p2 = a + (b - a) * fi0 + (c - a) * fj1;
                        glm::vec3 wp0 = glm::vec3(trsMat * glm::vec4(p0, 1.f));
                        glm::vec3 wp1 = glm::vec3(trsMat * glm::vec4(p1, 1.f));
                        glm::vec3 wp2 = glm::vec3(trsMat * glm::vec4(p2, 1.f));
                        tris.push_back({wp0.x, wp0.y, wp0.z, wp1.x, wp1.y, wp1.z, wp2.x, wp2.y, wp2.z});
                        if (i + j < n - 1) {
                            glm::vec3 p3 = a + (b - a) * fi1 + (c - a) * fj1;
                            glm::vec3 wp3 = glm::vec3(trsMat * glm::vec4(p3, 1.f));
                            tris.push_back({wp1.x, wp1.y, wp1.z, wp3.x, wp3.y, wp3.z, wp2.x, wp2.y, wp2.z});
                        }
                    }
                }
            };
            // Bottom
            pushTri(v[0], v[1], v[5], subdiv);
            pushTri(v[0], v[5], v[2], subdiv);
            // Slope
            pushTri(v[0], v[1], v[4], subdiv);
            pushTri(v[0], v[4], v[3], subdiv);
            // Right face
            pushTri(v[2], v[5], v[4], subdiv);
            pushTri(v[2], v[4], v[3], subdiv);
            // Triangular caps
            pushTri(v[0], v[2], v[3], subdiv);
            pushTri(v[1], v[5], v[4], subdiv);
        };

        for (auto& sc : scenery) {
            if (sc.collision == 0) continue;
            sqlite3_bind_int(stmt, 1, sc.modelId);

            // Full TRS matrix (pitch=X, yaw=Y, roll=Z in degrees)
            glm::mat4 T   = glm::translate(glm::mat4(1.f), sc.pos);
            glm::mat4 Ry  = glm::rotate(glm::mat4(1.f), glm::radians(sc.rot.y), glm::vec3(0,1,0));
            glm::mat4 Rx  = glm::rotate(glm::mat4(1.f), glm::radians(sc.rot.x), glm::vec3(1,0,0));
            glm::mat4 Rz  = glm::rotate(glm::mat4(1.f), glm::radians(sc.rot.z), glm::vec3(0,0,1));
            glm::mat4 S   = glm::scale(glm::mat4(1.f), sc.scale);
            glm::mat4 trs = T * Ry * Rx * Rz * S;

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int   type = sqlite3_column_int(stmt, 0);
                bool allow = false;
                switch (sc.collision) {
                case 1: allow = (type == 1); break; // sphere
                case 2: allow = (type == 0 || type == 3); break; // box / wedge
                case 3: allow = (type == 2); break; // polygon / mesh
                default: allow = true; break;       // legacy / unknown
                }
                if (!allow) continue;
                float ox   = (float)sqlite3_column_double(stmt, 1);
                float oy   = (float)sqlite3_column_double(stmt, 2);
                float oz   = (float)sqlite3_column_double(stmt, 3);
                float sx   = (float)sqlite3_column_double(stmt, 4);
                float sy   = (float)sqlite3_column_double(stmt, 5);
                float sz   = (float)sqlite3_column_double(stmt, 6);
                float detailA = (float)sqlite3_column_double(stmt, 7);

                if (type == 0) { // Box — scale only (AABB stays axis-aligned)
                    float cx = sc.pos.x + ox * sc.scale.x;
                    float cy = sc.pos.y + oy * sc.scale.y;
                    float cz = sc.pos.z + oz * sc.scale.z;
                    boxes.push_back({cx, cy, cz,
                                     sx*0.5f*sc.scale.x,
                                     sy*0.5f*sc.scale.y,
                                     sz*0.5f*sc.scale.z});
                } else if (type == 1) { // Sphere
                    float cx = sc.pos.x + ox * sc.scale.x;
                    float cy = sc.pos.y + oy * sc.scale.y;
                    float cz = sc.pos.z + oz * sc.scale.z;
                    float maxScale = std::max({sc.scale.x, sc.scale.y, sc.scale.z});
                    spheres.push_back({cx, cy, cz, sx * maxScale});
                } else if (type == 3) { // Wedge / ramp
                    int subdiv = (int)std::lround(detailA);
                    if (subdiv < 1) subdiv = 1;
                    if (subdiv > 16) subdiv = 16;
                    appendWedgeTris(ox, oy, oz, sx, sy, sz, subdiv, trs);
                } else { // Mesh — extract triangles and apply full TRS
                    if (pathStmt) {
                        sqlite3_bind_int(pathStmt, 1, sc.modelId);
                        if (sqlite3_step(pathStmt) == SQLITE_ROW) {
                            const char* rel = (const char*)sqlite3_column_text(pathStmt, 0);
                            if (rel && rel[0]) {
                                char fullPath[512];
                                std::snprintf(fullPath, sizeof(fullPath), "../client/%s", rel);
                                std::vector<std::array<glm::vec3, 3>> meshTris;
                                rco::renderer::ExtractMeshTriangles(fullPath, meshTris);
                                float budgetPct = detailA > 0.f ? detailA : 100.f;
                                if (budgetPct < 0.1f) budgetPct = 0.1f;
                                if (budgetPct > 100.f) budgetPct = 100.f;
                                std::vector<std::array<glm::vec3, 3>> reduced =
                                    SimplifyTrianglesForBudget(meshTris, budgetPct);
                                for (auto& tri : reduced) {
                                    glm::vec3 a = glm::vec3(trs * glm::vec4(tri[0], 1.f));
                                    glm::vec3 b = glm::vec3(trs * glm::vec4(tri[1], 1.f));
                                    glm::vec3 c = glm::vec3(trs * glm::vec4(tri[2], 1.f));
                                    tris.push_back({a.x,a.y,a.z, b.x,b.y,b.z, c.x,c.y,c.z});
                                }
                            }
                        }
                        sqlite3_reset(pathStmt);
                    }
                }
            }
            sqlite3_reset(stmt);
        }
        if (pathStmt) sqlite3_finalize(pathStmt);
        sqlite3_finalize(stmt);
    }

    w32(0x444C4F43u); // 'COLD'
    w32(2);           // version

    w32((uint32_t)boxes.size());
    for (auto& b : boxes) { wf(b.cx); wf(b.cy); wf(b.cz); wf(b.hx); wf(b.hy); wf(b.hz); }

    w32((uint32_t)spheres.size());
    for (auto& s : spheres) { wf(s.cx); wf(s.cy); wf(s.cz); wf(s.r); }

    w32((uint32_t)tris.size());
    for (auto& t : tris) {
        wf(t.ax); wf(t.ay); wf(t.az);
        wf(t.bx); wf(t.by); wf(t.bz);
        wf(t.cx_); wf(t.cy_); wf(t.cz_);
    }

    std::fclose(f);
}

// ─── RebuildColVis ────────────────────────────────────────────────────────────

namespace {

// Box wireframe edges (12 edges × 2 vertices).
// kBoxCorners matches the unit cube from -0.5 to 0.5 used in the VAO.
static const int kBoxEdges[12][2] = {
    {0,1},{1,2},{2,3},{3,0}, // front face
    {4,5},{5,6},{6,7},{7,4}, // back face
    {0,4},{1,5},{2,6},{3,7}, // connecting
};

void AppendBox(ColVisData& vis, const glm::vec3& center, const glm::vec3& scale,
               float r, float g, float b, float a)
{
    // 8 corners: combine ±0.5 × scale + center
    glm::vec3 corners[8];
    for (int i = 0; i < 8; ++i)
        corners[i] = center + glm::vec3(
            scale.x * ((i & 1) ? 0.5f : -0.5f),
            scale.y * ((i & 2) ? 0.5f : -0.5f),
            scale.z * ((i & 4) ? 0.5f : -0.5f));
    for (auto& e : kBoxEdges) {
        vis.verts.push_back({corners[e[0]].x, corners[e[0]].y, corners[e[0]].z, r,g,b,a});
        vis.verts.push_back({corners[e[1]].x, corners[e[1]].y, corners[e[1]].z, r,g,b,a});
    }
}

void AppendSphere(ColVisData& vis, const glm::vec3& center, float radius,
                  float r, float g, float b, float a)
{
    constexpr int kSegs = 24;
    constexpr float kStep = 2.f * 3.14159265f / kSegs;
    // XZ circle
    for (int i = 0; i < kSegs; ++i) {
        float a0 = i * kStep, a1 = (i + 1) * kStep;
        float x0 = center.x + radius * std::cos(a0), z0 = center.z + radius * std::sin(a0);
        float x1 = center.x + radius * std::cos(a1), z1 = center.z + radius * std::sin(a1);
        vis.verts.push_back({x0, center.y, z0, r,g,b,a});
        vis.verts.push_back({x1, center.y, z1, r,g,b,a});
    }
    // XY circle
    for (int i = 0; i < kSegs; ++i) {
        float a0 = i * kStep, a1 = (i + 1) * kStep;
        float x0 = center.x + radius * std::cos(a0), y0 = center.y + radius * std::sin(a0);
        float x1 = center.x + radius * std::cos(a1), y1 = center.y + radius * std::sin(a1);
        vis.verts.push_back({x0, y0, center.z, r,g,b,a});
        vis.verts.push_back({x1, y1, center.z, r,g,b,a});
    }
}

void AppendTris(ColVisData& vis,
                const std::vector<std::array<glm::vec3, 3>>& localTris,
                const glm::mat4& trs,
                float r, float g, float b, float a)
{
    for (auto& tri : localTris) {
        glm::vec3 v0 = glm::vec3(trs * glm::vec4(tri[0], 1.f));
        glm::vec3 v1 = glm::vec3(trs * glm::vec4(tri[1], 1.f));
        glm::vec3 v2 = glm::vec3(trs * glm::vec4(tri[2], 1.f));
        vis.verts.push_back({v0.x, v0.y, v0.z, r,g,b,a});
        vis.verts.push_back({v1.x, v1.y, v1.z, r,g,b,a});
        vis.verts.push_back({v1.x, v1.y, v1.z, r,g,b,a});
        vis.verts.push_back({v2.x, v2.y, v2.z, r,g,b,a});
        vis.verts.push_back({v2.x, v2.y, v2.z, r,g,b,a});
        vis.verts.push_back({v0.x, v0.y, v0.z, r,g,b,a});
    }
}

void BuildWedgeVerts(const glm::vec3& center, const glm::vec3& size, glm::vec3 out[6]) {
    const glm::vec3 h = glm::abs(size) * 0.5f;
    const bool risePositiveX = size.x >= 0.f;
    const float xLow  = risePositiveX ? (center.x - h.x) : (center.x + h.x);
    const float xHigh = risePositiveX ? (center.x + h.x) : (center.x - h.x);
    out[0] = {xLow,  center.y - h.y, center.z - h.z}; // low-back  bottom
    out[1] = {xLow,  center.y - h.y, center.z + h.z}; // low-front bottom
    out[2] = {xHigh, center.y - h.y, center.z - h.z}; // high-back bottom
    out[3] = {xHigh, center.y + h.y, center.z - h.z}; // high-back top
    out[4] = {xHigh, center.y + h.y, center.z + h.z}; // high-front top
    out[5] = {xHigh, center.y - h.y, center.z + h.z}; // high-front bottom
}

void AppendWedgeWire(ColVisData& vis, const glm::vec3& center, const glm::vec3& size,
                     const glm::mat4& trs, int subdiv, float r, float g, float b, float a)
{
    glm::vec3 local[6];
    BuildWedgeVerts(center, size, local);
    glm::vec3 w[6];
    for (int i = 0; i < 6; ++i)
        w[i] = glm::vec3(trs * glm::vec4(local[i], 1.f));

    static const int kEdges[9][2] = {
        {0,1}, {0,2}, {1,5}, {2,5},
        {2,3}, {5,4}, {3,4}, {0,3}, {1,4},
    };
    for (auto& e : kEdges) {
        vis.verts.push_back({w[e[0]].x, w[e[0]].y, w[e[0]].z, r,g,b,a});
        vis.verts.push_back({w[e[1]].x, w[e[1]].y, w[e[1]].z, r,g,b,a});
    }

    int n = subdiv;
    if (n < 1) n = 1;
    if (n == 1) return;
    auto addSubdivTriEdges = [&](const glm::vec3& a0, const glm::vec3& b0, const glm::vec3& c0) {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n - i; ++j) {
                float fi0 = (float)i / (float)n;
                float fj0 = (float)j / (float)n;
                float fi1 = (float)(i + 1) / (float)n;
                float fj1 = (float)(j + 1) / (float)n;
                glm::vec3 p0 = a0 + (b0 - a0) * fi0 + (c0 - a0) * fj0;
                glm::vec3 p1 = a0 + (b0 - a0) * fi1 + (c0 - a0) * fj0;
                glm::vec3 p2 = a0 + (b0 - a0) * fi0 + (c0 - a0) * fj1;
                vis.verts.push_back({p0.x, p0.y, p0.z, r,g,b,a * 0.55f});
                vis.verts.push_back({p1.x, p1.y, p1.z, r,g,b,a * 0.55f});
                vis.verts.push_back({p1.x, p1.y, p1.z, r,g,b,a * 0.55f});
                vis.verts.push_back({p2.x, p2.y, p2.z, r,g,b,a * 0.55f});
                vis.verts.push_back({p2.x, p2.y, p2.z, r,g,b,a * 0.55f});
                vis.verts.push_back({p0.x, p0.y, p0.z, r,g,b,a * 0.55f});
                if (i + j < n - 1) {
                    glm::vec3 p3 = a0 + (b0 - a0) * fi1 + (c0 - a0) * fj1;
                    vis.verts.push_back({p1.x, p1.y, p1.z, r,g,b,a * 0.55f});
                    vis.verts.push_back({p3.x, p3.y, p3.z, r,g,b,a * 0.55f});
                    vis.verts.push_back({p3.x, p3.y, p3.z, r,g,b,a * 0.55f});
                    vis.verts.push_back({p2.x, p2.y, p2.z, r,g,b,a * 0.55f});
                    vis.verts.push_back({p2.x, p2.y, p2.z, r,g,b,a * 0.55f});
                    vis.verts.push_back({p1.x, p1.y, p1.z, r,g,b,a * 0.55f});
                }
            }
        }
    };
    static const int kTri[8][3] = {
        {0,1,5}, {0,5,2},
        {0,1,4}, {0,4,3},
        {2,5,4}, {2,4,3},
        {0,2,3}, {1,5,4},
    };
    for (auto& t : kTri) {
        addSubdivTriEdges(w[t[0]], w[t[1]], w[t[2]]);
    }
}

} // anonymous namespace

void ZoneScene::RebuildColVis(sqlite3* db, MeshTriCache& meshCache) {
    colVis      = {};
    colVisDirty = false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT type, offset_x, offset_y, offset_z, size_x, size_y, size_z, detail_a, detail_b"
        " FROM media_model_shapes WHERE model_id=? ORDER BY id",
        -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_stmt* pathStmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT file_path FROM media_models WHERE id=?",
        -1, &pathStmt, nullptr);

    for (auto& sc : scenery) {
        if (sc.collision == 0) continue;
        sqlite3_bind_int(stmt, 1, sc.modelId);

        glm::mat4 T   = glm::translate(glm::mat4(1.f), sc.pos);
        glm::mat4 Ry  = glm::rotate(glm::mat4(1.f), glm::radians(sc.rot.y), glm::vec3(0,1,0));
        glm::mat4 Rx  = glm::rotate(glm::mat4(1.f), glm::radians(sc.rot.x), glm::vec3(1,0,0));
        glm::mat4 Rz  = glm::rotate(glm::mat4(1.f), glm::radians(sc.rot.z), glm::vec3(0,0,1));
        glm::mat4 S   = glm::scale(glm::mat4(1.f), sc.scale);
        glm::mat4 trs = T * Ry * Rx * Rz * S;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int   type = sqlite3_column_int(stmt, 0);
            bool allow = false;
            switch (sc.collision) {
            case 1: allow = (type == 1); break; // sphere
            case 2: allow = (type == 0 || type == 3); break; // box / wedge
            case 3: allow = (type == 2); break; // polygon / mesh
            default: allow = true; break;       // legacy / unknown
            }
            if (!allow) continue;
            float ox   = (float)sqlite3_column_double(stmt, 1);
            float oy   = (float)sqlite3_column_double(stmt, 2);
            float oz   = (float)sqlite3_column_double(stmt, 3);
            float sx   = (float)sqlite3_column_double(stmt, 4);
            float sy   = (float)sqlite3_column_double(stmt, 5);
            float sz   = (float)sqlite3_column_double(stmt, 6);
            float detailA = (float)sqlite3_column_double(stmt, 7);

            if (type == 0) { // Box
                glm::vec3 center = {sc.pos.x + ox * sc.scale.x,
                                    sc.pos.y + oy * sc.scale.y,
                                    sc.pos.z + oz * sc.scale.z};
                AppendBox(colVis, center,
                          {sx * sc.scale.x, sy * sc.scale.y, sz * sc.scale.z},
                          1.f, 0.25f, 0.25f, 0.8f);
            } else if (type == 1) { // Sphere
                glm::vec3 center = {sc.pos.x + ox * sc.scale.x,
                                    sc.pos.y + oy * sc.scale.y,
                                    sc.pos.z + oz * sc.scale.z};
                float maxScale = std::max({sc.scale.x, sc.scale.y, sc.scale.z});
                AppendSphere(colVis, center, sx * maxScale,
                             1.f, 0.55f, 0.1f, 0.8f);
            } else if (type == 3) { // Wedge / ramp
                int subdiv = (int)std::lround(detailA);
                if (subdiv < 1) subdiv = 1;
                if (subdiv > 16) subdiv = 16;
                AppendWedgeWire(colVis,
                                {ox, oy, oz},
                                {sx, sy, sz},
                                trs,
                                subdiv,
                                0.67f, 1.f, 0.43f, 0.9f);
            } else { // Mesh
                auto it = meshCache.find(sc.modelId);
                if (it == meshCache.end()) {
                    if (pathStmt) {
                        sqlite3_bind_int(pathStmt, 1, sc.modelId);
                        if (sqlite3_step(pathStmt) == SQLITE_ROW) {
                            const char* rel = (const char*)sqlite3_column_text(pathStmt, 0);
                            if (rel && rel[0]) {
                                char fp[512];
                                std::snprintf(fp, sizeof(fp), "../client/%s", rel);
                                std::vector<std::array<glm::vec3, 3>> localTris;
                                rco::renderer::ExtractMeshTriangles(fp, localTris);
                                meshCache[sc.modelId] = std::move(localTris);
                            }
                        }
                        sqlite3_reset(pathStmt);
                        it = meshCache.find(sc.modelId);
                    }
                }
                if (it != meshCache.end()) {
                    float budgetPct = detailA > 0.f ? detailA : 100.f;
                    if (budgetPct < 0.1f) budgetPct = 0.1f;
                    if (budgetPct > 100.f) budgetPct = 100.f;
                    std::vector<std::array<glm::vec3, 3>> reduced =
                        SimplifyTrianglesForBudget(it->second, budgetPct);
                    AppendTris(colVis, reduced, trs, 1.f, 0.45f, 0.f, 0.8f);
                }
            }
        }
        sqlite3_reset(stmt);
    }
    if (pathStmt) sqlite3_finalize(pathStmt);
    sqlite3_finalize(stmt);
}

} // namespace gue
