#include "zone_scene.h"
#include <cstdio>

namespace gue {

// Helper — run a DDL statement, silently ignore "duplicate column" errors.
static void Exec(sqlite3* db, const char* sql) {
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

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
        "  locked       INTEGER NOT NULL DEFAULT 0"
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

    // ── Water ─────────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, x, y, z, scale_x, scale_z,"
        "       color_r, color_g, color_b, opacity,"
        "       tex_path, tex_scale, damage, dmg_type"
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
            water.push_back(w);
        }
        sqlite3_finalize(stmt);
    }

    // ── Scenery ───────────────────────────────────────────────────────────
    if (sqlite3_prepare_v2(db,
        "SELECT id, model_id, material_id,"
        "       x, y, z, pitch, yaw, roll, sx, sy, sz,"
        "       collision, anim_mode, inv_size, ownable, locked"
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
            scenery.push_back(s);
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

} // namespace gue
