#include "areas.h"
#include <imgui.h>
#include <cstring>
#include <cstdio>

namespace gue {

static const char* kMusicNames[] = {
    "0 - Stop",
    "1 - Starter Zone",
    "2 - Forest",
    "3 - Combat",
};
static constexpr int kMusicCount = 4;

// ---------------------------------------------------------------------------
// Table setup
// ---------------------------------------------------------------------------

void AreasTab::EnsureTables(sqlite3* db) {
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS area_config ("
        "  name        TEXT PRIMARY KEY,"
        "  music_track INTEGER NOT NULL DEFAULT 1,"
        "  fog_density REAL    NOT NULL DEFAULT 0.0"
        ")",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE area_config ADD COLUMN skybox_hdr TEXT NOT NULL DEFAULT ''",
        nullptr, nullptr, nullptr); // ignored if column already exists

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS area_portals ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name   TEXT    NOT NULL DEFAULT '',"
        "  x           REAL    NOT NULL DEFAULT 0,"
        "  z           REAL    NOT NULL DEFAULT 0,"
        "  radius      REAL    NOT NULL DEFAULT 3,"
        "  target_area TEXT    NOT NULL DEFAULT '',"
        "  dest_x      REAL    NOT NULL DEFAULT 0,"
        "  dest_y      REAL    NOT NULL DEFAULT 0,"
        "  dest_z      REAL    NOT NULL DEFAULT 0,"
        "  dest_yaw    REAL    NOT NULL DEFAULT 0"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS area_waypoints ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name TEXT NOT NULL DEFAULT '',"
        "  x         REAL NOT NULL DEFAULT 0,"
        "  y         REAL NOT NULL DEFAULT 0,"
        "  z         REAL NOT NULL DEFAULT 0,"
        "  next_a    INTEGER NOT NULL DEFAULT 0,"
        "  next_b    INTEGER NOT NULL DEFAULT 0,"
        "  pause_ms  INTEGER NOT NULL DEFAULT 0"
        ")",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_waypoints_area ON area_waypoints(area_name)",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS world_objects ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  area_name TEXT    NOT NULL DEFAULT '',"
        "  model_id  INTEGER NOT NULL DEFAULT 0,"
        "  x         REAL    NOT NULL DEFAULT 0,"
        "  y         REAL    NOT NULL DEFAULT 0,"
        "  z         REAL    NOT NULL DEFAULT 0,"
        "  yaw       REAL    NOT NULL DEFAULT 0,"
        "  scale     REAL    NOT NULL DEFAULT 1"
        ")",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_world_objects_area ON world_objects(area_name)",
        nullptr, nullptr, nullptr);

    // Seed default areas if none exist.
    int count = 0;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM area_config", -1, &s, nullptr) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) count = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (count == 0) {
        sqlite3_exec(db,
            "INSERT OR IGNORE INTO area_config (name, music_track) VALUES"
            " ('Starter Zone', 1),"
            " ('Forest', 2)",
            nullptr, nullptr, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Fetch
// ---------------------------------------------------------------------------

void AreasTab::FetchAreas(sqlite3* db) {
    EnsureTables(db);
    areas_.clear();
    selectedArea_        = -1;
    selectedPortal_      = -1;
    selectedWaypoint_    = -1;
    selectedWorldObject_ = -1;
    portals_.clear();
    waypoints_.clear();
    world_objects_.clear();
    wo_models_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT name, music_track, fog_density, skybox_hdr FROM area_config ORDER BY name",
            -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Fetch error: %s", sqlite3_errmsg(db));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AreaConfig a;
        a.name        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        a.music_track = sqlite3_column_int(stmt, 1);
        a.fog_density = static_cast<float>(sqlite3_column_double(stmt, 2));
        if (auto* s = sqlite3_column_text(stmt, 3)) a.skybox_hdr = reinterpret_cast<const char*>(s);
        areas_.push_back(a);
    }
    sqlite3_finalize(stmt);
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Loaded %d areas.", (int)areas_.size());
}

void AreasTab::FetchWaypoints(sqlite3* db, const std::string& area) {
    waypoints_.clear();
    selectedWaypoint_ = -1;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT id, area_name, x, y, z, next_a, next_b, pause_ms"
            " FROM area_waypoints WHERE area_name=? ORDER BY id",
            -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AreaWaypoint w;
        w.id        = sqlite3_column_int(stmt, 0);
        w.area_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        w.x         = static_cast<float>(sqlite3_column_double(stmt, 2));
        w.y         = static_cast<float>(sqlite3_column_double(stmt, 3));
        w.z         = static_cast<float>(sqlite3_column_double(stmt, 4));
        w.next_a    = sqlite3_column_int(stmt, 5);
        w.next_b    = sqlite3_column_int(stmt, 6);
        w.pause_ms  = sqlite3_column_int(stmt, 7);
        waypoints_.push_back(w);
    }
    sqlite3_finalize(stmt);
}

void AreasTab::FetchPortals(sqlite3* db, const std::string& area) {
    portals_.clear();
    selectedPortal_ = -1;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT id, area_name, x, z, radius, target_area, dest_x, dest_y, dest_z, dest_yaw"
            " FROM area_portals WHERE area_name=? ORDER BY id",
            -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AreaPortal p;
        p.id          = sqlite3_column_int(stmt, 0);
        p.area_name   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        p.x           = static_cast<float>(sqlite3_column_double(stmt, 2));
        p.z           = static_cast<float>(sqlite3_column_double(stmt, 3));
        p.radius      = static_cast<float>(sqlite3_column_double(stmt, 4));
        p.target_area = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        p.dest_x      = static_cast<float>(sqlite3_column_double(stmt, 6));
        p.dest_y      = static_cast<float>(sqlite3_column_double(stmt, 7));
        p.dest_z      = static_cast<float>(sqlite3_column_double(stmt, 8));
        p.dest_yaw    = static_cast<float>(sqlite3_column_double(stmt, 9));
        portals_.push_back(p);
    }
    sqlite3_finalize(stmt);
}

// ---------------------------------------------------------------------------
// Save / Delete
// ---------------------------------------------------------------------------

bool AreasTab::SaveArea(sqlite3* db, AreaConfig& a, bool isNew) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = isNew
        ? "INSERT OR REPLACE INTO area_config (name, music_track, fog_density, skybox_hdr) VALUES (?,?,?,?)"
        : "UPDATE area_config SET music_track=?, fog_density=?, skybox_hdr=? WHERE name=?";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Save error: %s", sqlite3_errmsg(db));
        return false;
    }
    if (isNew) {
        sqlite3_bind_text(stmt, 1, a.name.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 2, a.music_track);
        sqlite3_bind_double(stmt, 3, a.fog_density);
        sqlite3_bind_text(stmt, 4, a.skybox_hdr.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_int (stmt, 1, a.music_track);
        sqlite3_bind_double(stmt, 2, a.fog_density);
        sqlite3_bind_text(stmt, 3, a.skybox_hdr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, a.name.c_str(),       -1, SQLITE_TRANSIENT);
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Save error: %s", sqlite3_errmsg(db));
        return false;
    }
    needFetchAreas_ = true;
    dirtyArea_      = false;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Saved area '%s'.", a.name.c_str());
    return true;
}

bool AreasTab::DeleteArea(sqlite3* db, const std::string& name) {
    // Delete portals in this area first.
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM area_portals WHERE area_name=?", -1, &s, nullptr);
    sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s); sqlite3_finalize(s);

    sqlite3_prepare_v2(db, "DELETE FROM area_config WHERE name=?", -1, &s, nullptr);
    sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    needFetchAreas_ = true;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted area '%s'.", name.c_str());
    return true;
}

bool AreasTab::SavePortal(sqlite3* db, AreaPortal& p) {
    sqlite3_stmt* stmt = nullptr;
    int rc;

    if (p.id == 0) {
        const char* sql =
            "INSERT INTO area_portals"
            " (area_name, x, z, radius, target_area, dest_x, dest_y, dest_z, dest_yaw)"
            " VALUES (?,?,?,?,?,?,?,?,?)";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt, 1, p.area_name.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, p.x);
        sqlite3_bind_double(stmt, 3, p.z);
        sqlite3_bind_double(stmt, 4, p.radius);
        sqlite3_bind_text(stmt, 5, p.target_area.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 6, p.dest_x);
        sqlite3_bind_double(stmt, 7, p.dest_y);
        sqlite3_bind_double(stmt, 8, p.dest_z);
        sqlite3_bind_double(stmt, 9, p.dest_yaw);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
        p.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        const char* sql =
            "UPDATE area_portals SET"
            " area_name=?, x=?, z=?, radius=?, target_area=?,"
            " dest_x=?, dest_y=?, dest_z=?, dest_yaw=?"
            " WHERE id=?";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt, 1, p.area_name.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, p.x);
        sqlite3_bind_double(stmt, 3, p.z);
        sqlite3_bind_double(stmt, 4, p.radius);
        sqlite3_bind_text(stmt, 5, p.target_area.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 6, p.dest_x);
        sqlite3_bind_double(stmt, 7, p.dest_y);
        sqlite3_bind_double(stmt, 8, p.dest_z);
        sqlite3_bind_double(stmt, 9, p.dest_yaw);
        sqlite3_bind_int(stmt, 10, p.id);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
    }
    sqlite3_finalize(stmt);
    needFetchPortals_ = true;
    dirtyPortal_      = false;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Saved portal %d.", p.id);
    return true;

err:
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Save error: %s", sqlite3_errmsg(db));
    if (stmt) sqlite3_finalize(stmt);
    return false;
}

bool AreasTab::DeletePortal(sqlite3* db, int id) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM area_portals WHERE id=?", -1, &s, nullptr);
    sqlite3_bind_int(s, 1, id);
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    needFetchPortals_ = true;
    selectedPortal_   = -1;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted portal %d.", id);
    return true;
}

bool AreasTab::SaveWaypoint(sqlite3* db, AreaWaypoint& w) {
    sqlite3_stmt* stmt = nullptr;
    int rc;

    if (w.id == 0) {
        const char* sql =
            "INSERT INTO area_waypoints (area_name,x,y,z,next_a,next_b,pause_ms)"
            " VALUES (?,?,?,?,?,?,?)";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt,   1, w.area_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, w.x);
        sqlite3_bind_double(stmt, 3, w.y);
        sqlite3_bind_double(stmt, 4, w.z);
        sqlite3_bind_int(stmt,    5, w.next_a);
        sqlite3_bind_int(stmt,    6, w.next_b);
        sqlite3_bind_int(stmt,    7, w.pause_ms);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
        w.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        const char* sql =
            "UPDATE area_waypoints SET"
            " area_name=?, x=?, y=?, z=?, next_a=?, next_b=?, pause_ms=?"
            " WHERE id=?";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt,   1, w.area_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, w.x);
        sqlite3_bind_double(stmt, 3, w.y);
        sqlite3_bind_double(stmt, 4, w.z);
        sqlite3_bind_int(stmt,    5, w.next_a);
        sqlite3_bind_int(stmt,    6, w.next_b);
        sqlite3_bind_int(stmt,    7, w.pause_ms);
        sqlite3_bind_int(stmt,    8, w.id);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
    }
    sqlite3_finalize(stmt);
    needFetchWaypoints_ = true;
    dirtyWaypoint_      = false;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Saved waypoint %d.", w.id);
    return true;

err:
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Save error: %s", sqlite3_errmsg(db));
    if (stmt) sqlite3_finalize(stmt);
    return false;
}

bool AreasTab::DeleteWaypoint(sqlite3* db, int id) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM area_waypoints WHERE id=?", -1, &s, nullptr);
    sqlite3_bind_int(s, 1, id);
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    needFetchWaypoints_ = true;
    selectedWaypoint_   = -1;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted waypoint %d.", id);
    return true;
}

void AreasTab::FetchWorldObjects(sqlite3* db, const std::string& area) {
    world_objects_.clear();
    selectedWorldObject_ = -1;

    // Load model list for the picker.
    wo_models_.clear();
    {
        sqlite3_stmt* ms = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT id, name FROM media_models ORDER BY name",
                               -1, &ms, nullptr) == SQLITE_OK) {
            while (sqlite3_step(ms) == SQLITE_ROW) {
                ModelEntry e;
                e.id   = sqlite3_column_int(ms, 0);
                e.name = reinterpret_cast<const char*>(sqlite3_column_text(ms, 1));
                wo_models_.push_back(e);
            }
            sqlite3_finalize(ms);
        }
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT id, area_name, model_id, x, y, z, yaw, scale"
            " FROM world_objects WHERE area_name=? ORDER BY id",
            -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, area.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AreaWorldObject w;
        w.id        = sqlite3_column_int(stmt, 0);
        w.area_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        w.model_id  = sqlite3_column_int(stmt, 2);
        w.x         = static_cast<float>(sqlite3_column_double(stmt, 3));
        w.y         = static_cast<float>(sqlite3_column_double(stmt, 4));
        w.z         = static_cast<float>(sqlite3_column_double(stmt, 5));
        w.yaw       = static_cast<float>(sqlite3_column_double(stmt, 6));
        w.scale     = static_cast<float>(sqlite3_column_double(stmt, 7));
        world_objects_.push_back(w);
    }
    sqlite3_finalize(stmt);
}

bool AreasTab::SaveWorldObject(sqlite3* db, AreaWorldObject& w) {
    sqlite3_stmt* stmt = nullptr;
    int rc;

    if (w.id == 0) {
        const char* sql =
            "INSERT INTO world_objects (area_name, model_id, x, y, z, yaw, scale)"
            " VALUES (?,?,?,?,?,?,?)";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text  (stmt, 1, w.area_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 2, w.model_id);
        sqlite3_bind_double(stmt, 3, w.x);
        sqlite3_bind_double(stmt, 4, w.y);
        sqlite3_bind_double(stmt, 5, w.z);
        sqlite3_bind_double(stmt, 6, w.yaw);
        sqlite3_bind_double(stmt, 7, w.scale);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
        w.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        const char* sql =
            "UPDATE world_objects SET area_name=?, model_id=?, x=?, y=?, z=?, yaw=?, scale=?"
            " WHERE id=?";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text  (stmt, 1, w.area_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 2, w.model_id);
        sqlite3_bind_double(stmt, 3, w.x);
        sqlite3_bind_double(stmt, 4, w.y);
        sqlite3_bind_double(stmt, 5, w.z);
        sqlite3_bind_double(stmt, 6, w.yaw);
        sqlite3_bind_double(stmt, 7, w.scale);
        sqlite3_bind_int   (stmt, 8, w.id);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
    }
    sqlite3_finalize(stmt);
    needFetchWorldObjects_ = true;
    dirtyWorldObject_      = false;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Saved world object %d.", w.id);
    return true;

err:
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Save error: %s", sqlite3_errmsg(db));
    if (stmt) sqlite3_finalize(stmt);
    return false;
}

bool AreasTab::DeleteWorldObject(sqlite3* db, int id) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM world_objects WHERE id=?", -1, &s, nullptr);
    sqlite3_bind_int(s, 1, id);
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    needFetchWorldObjects_ = true;
    selectedWorldObject_   = -1;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted world object %d.", id);
    return true;
}

// ---------------------------------------------------------------------------
// Draw helpers
// ---------------------------------------------------------------------------

static bool DrawAreaFields(AreaConfig& a) {
    bool changed = false;
    if (a.music_track < 0 || a.music_track >= kMusicCount) a.music_track = 0;
    if (ImGui::Combo("Music Track", &a.music_track, kMusicNames, kMusicCount)) changed = true;
    if (ImGui::SliderFloat("Fog Density", &a.fog_density, 0.f, 1.f, "%.3f")) changed = true;

    // Skybox HDR — filename relative to assets/ibl/. Leave empty to use default.hdr.
    char hdrBuf[256];
    std::snprintf(hdrBuf, sizeof(hdrBuf), "%s", a.skybox_hdr.c_str());
    if (ImGui::InputText("Skybox HDR", hdrBuf, sizeof(hdrBuf))) {
        a.skybox_hdr = hdrBuf;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Filename inside assets/ibl/ (e.g. forest.hdr).\nLeave empty to use default.hdr.");
    return changed;
}

static bool DrawPortalFields(AreaPortal& p, const std::vector<AreaConfig>& areas) {
    bool changed = false;
    char buf[128];

    ImGui::TextUnformatted("Trigger Volume (XZ plane)");
    float colW = (ImGui::GetContentRegionAvail().x - 12.f) / 3.f;
    ImGui::SetNextItemWidth(colW);
    if (ImGui::InputFloat("X##trig", &p.x, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(colW);
    if (ImGui::InputFloat("Z##trig", &p.z, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputFloat("Radius##trig", &p.radius, 0.f, 0.f, "%.2f")) changed = true;
    if (p.radius < 0.5f) p.radius = 0.5f;

    ImGui::Spacing();
    ImGui::TextUnformatted("Destination");

    // Target area combo from known areas.
    int targetIdx = -1;
    for (int i = 0; i < (int)areas.size(); ++i)
        if (areas[i].name == p.target_area) { targetIdx = i; break; }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("Target Area", p.target_area.empty() ? "(select)" : p.target_area.c_str())) {
        for (int i = 0; i < (int)areas.size(); ++i) {
            bool sel = (i == targetIdx);
            if (ImGui::Selectable(areas[i].name.c_str(), sel)) {
                p.target_area = areas[i].name;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    float w4 = (ImGui::GetContentRegionAvail().x - 12.f) / 4.f;
    ImGui::SetNextItemWidth(w4);
    if (ImGui::InputFloat("X##dest", &p.dest_x, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w4);
    if (ImGui::InputFloat("Y##dest", &p.dest_y, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w4);
    if (ImGui::InputFloat("Z##dest", &p.dest_z, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputFloat("Yaw##dest", &p.dest_yaw, 0.f, 0.f, "%.1f")) changed = true;

    return changed;
}

static bool DrawWaypointFields(AreaWaypoint& w) {
    bool changed = false;
    float w3 = (ImGui::GetContentRegionAvail().x - 8.f) / 3.f;
    ImGui::SetNextItemWidth(w3);
    if (ImGui::InputFloat("X##wp", &w.x, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w3);
    if (ImGui::InputFloat("Y##wp", &w.y, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputFloat("Z##wp", &w.z, 0.f, 0.f, "%.2f")) changed = true;

    ImGui::Spacing();
    ImGui::TextUnformatted("Successor Waypoints (0 = end)");
    float w2 = (ImGui::GetContentRegionAvail().x - 8.f) / 2.f;
    ImGui::SetNextItemWidth(w2);
    if (ImGui::InputInt("Next A##wp", &w.next_a)) { if (w.next_a < 0) w.next_a = 0; changed = true; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("Next B##wp", &w.next_b)) { if (w.next_b < 0) w.next_b = 0; changed = true; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("Pause ms##wp", &w.pause_ms)) { if (w.pause_ms < 0) w.pause_ms = 0; changed = true; }
    return changed;
}

// ---------------------------------------------------------------------------
// Draw (main entry)
// ---------------------------------------------------------------------------

void AreasTab::Draw(sqlite3* db) {
    if (needFetchAreas_) { FetchAreas(db); needFetchAreas_ = false; }

    if (selectedArea_ >= 0 && selectedArea_ < (int)areas_.size()) {
        const auto& areaName = areas_[selectedArea_].name;
        if (needFetchPortals_)      { FetchPortals(db, areaName);      needFetchPortals_      = false; }
        if (needFetchWaypoints_)    { FetchWaypoints(db, areaName);    needFetchWaypoints_    = false; }
        if (needFetchWorldObjects_) { FetchWorldObjects(db, areaName); needFetchWorldObjects_ = false; }
    }

    if (ImGui::Button("Refresh"))     needFetchAreas_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Area")) {
        newArea_      = {};
        showNewArea_  = true;
        selectedArea_ = -1;
        portals_.clear();
        waypoints_.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    // ── Left: area list ────────────────────────────────────────────────────
    float listW = 180.f;
    ImGui::BeginChild("##area_list", {listW, 0}, true);
    for (int i = 0; i < (int)areas_.size(); ++i) {
        auto& a = areas_[i];
        char label[128];
        std::snprintf(label, sizeof(label), "%s##al%d", a.name.c_str(), i);
        if (ImGui::Selectable(label, selectedArea_ == i)) {
            selectedArea_          = i;
            editingArea_           = a;
            dirtyArea_             = false;
            showNewArea_           = false;
            showNewPortal_         = false;
            showNewWaypoint_       = false;
            showNewWorldObject_    = false;
            selectedPortal_        = -1;
            selectedWaypoint_      = -1;
            selectedWorldObject_   = -1;
            needFetchPortals_      = true;
            needFetchWaypoints_    = true;
            needFetchWorldObjects_ = true;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right: details + portals ───────────────────────────────────────────
    ImGui::BeginChild("##area_edit", {0, 0}, false);

    // --- New area form ---
    if (showNewArea_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Area");
        ImGui::Separator();
        char nameBuf[128] = {};
        std::strncpy(nameBuf, newArea_.name.c_str(), sizeof(nameBuf)-1);
        if (ImGui::InputText("Name##newarea", nameBuf, sizeof(nameBuf)))
            newArea_.name = nameBuf;
        DrawAreaFields(newArea_);
        ImGui::Spacing();
        if (ImGui::Button("Create")) {
            if (!newArea_.name.empty() && SaveArea(db, newArea_, true)) {
                showNewArea_ = false;
            } else if (newArea_.name.empty()) {
                std::snprintf(statusMsg_, sizeof(statusMsg_), "Area name cannot be empty.");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) showNewArea_ = false;

    } else if (selectedArea_ >= 0 && selectedArea_ < (int)areas_.size()) {

        // --- Area settings panel ---
        ImGui::Text("Area: %s", editingArea_.name.c_str());
        ImGui::Separator();
        if (DrawAreaFields(editingArea_)) dirtyArea_ = true;

        ImGui::Spacing();
        ImGui::BeginDisabled(!dirtyArea_);
        if (ImGui::Button("Save Area")) SaveArea(db, editingArea_, false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert Area")) { editingArea_ = areas_[selectedArea_]; dirtyArea_ = false; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete Area")) {
            DeleteArea(db, editingArea_.name);
            showNewArea_ = false;
        }
        ImGui::PopStyleColor();

        // --- Portals section ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Portals");
        ImGui::SameLine();
        if (ImGui::SmallButton("New Portal")) {
            newPortal_      = {};
            newPortal_.area_name = editingArea_.name;
            showNewPortal_  = true;
            selectedPortal_ = -1;
        }
        ImGui::Separator();

        // Portal list (compact)
        float portalListH = std::min(120.f, 28.f * (float)(portals_.size() + 1));
        ImGui::BeginChild("##portal_list", {0, portalListH}, true);
        for (int i = 0; i < (int)portals_.size(); ++i) {
            auto& p = portals_[i];
            char label[200];
            std::snprintf(label, sizeof(label), "[%d] (%.0f, %.0f) r=%.0f → %s##pl%d",
                          p.id, p.x, p.z, p.radius, p.target_area.c_str(), i);
            if (ImGui::Selectable(label, selectedPortal_ == i)) {
                selectedPortal_ = i;
                editingPortal_  = p;
                dirtyPortal_    = false;
                showNewPortal_  = false;
            }
        }
        if (portals_.empty())
            ImGui::TextDisabled("No portals in this area.");
        ImGui::EndChild();

        // New portal form
        if (showNewPortal_) {
            ImGui::Spacing();
            ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Portal");
            ImGui::Separator();
            DrawPortalFields(newPortal_, areas_);
            ImGui::Spacing();
            if (ImGui::Button("Create Portal")) {
                if (SavePortal(db, newPortal_)) {
                    FetchPortals(db, editingArea_.name);
                    needFetchPortals_ = false;
                    showNewPortal_    = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##np")) showNewPortal_ = false;

        } else if (selectedPortal_ >= 0 && selectedPortal_ < (int)portals_.size()) {
            ImGui::Spacing();
            ImGui::Text("Portal [id=%d]", editingPortal_.id);
            ImGui::Separator();
            if (DrawPortalFields(editingPortal_, areas_)) dirtyPortal_ = true;
            ImGui::Spacing();

            ImGui::BeginDisabled(!dirtyPortal_);
            if (ImGui::Button("Save Portal")) {
                if (SavePortal(db, editingPortal_)) {
                    FetchPortals(db, editingArea_.name);
                    needFetchPortals_ = false;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Portal")) {
                editingPortal_ = portals_[selectedPortal_];
                dirtyPortal_   = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
            if (ImGui::Button("Delete Portal")) {
                DeletePortal(db, editingPortal_.id);
                FetchPortals(db, editingArea_.name);
                needFetchPortals_ = false;
            }
            ImGui::PopStyleColor();
        }

        // --- Waypoints section ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Waypoints");
        ImGui::SameLine();
        if (ImGui::SmallButton("New Waypoint")) {
            newWaypoint_           = {};
            newWaypoint_.area_name = editingArea_.name;
            showNewWaypoint_       = true;
            selectedWaypoint_      = -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(ID shown — use it as NPC Start Waypoint)");
        ImGui::Separator();

        float wpListH = std::min(120.f, 28.f * (float)(waypoints_.size() + 1));
        ImGui::BeginChild("##wp_list", {0, wpListH}, true);
        for (int i = 0; i < (int)waypoints_.size(); ++i) {
            auto& w = waypoints_[i];
            char label[200];
            std::snprintf(label, sizeof(label),
                "[%d] (%.0f,%.0f,%.0f) A=%d B=%d pause=%dms##wpl%d",
                w.id, w.x, w.y, w.z, w.next_a, w.next_b, w.pause_ms, i);
            if (ImGui::Selectable(label, selectedWaypoint_ == i)) {
                selectedWaypoint_ = i;
                editingWaypoint_  = w;
                dirtyWaypoint_    = false;
                showNewWaypoint_  = false;
            }
        }
        if (waypoints_.empty())
            ImGui::TextDisabled("No waypoints in this area.");
        ImGui::EndChild();

        if (showNewWaypoint_) {
            ImGui::Spacing();
            ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Waypoint");
            ImGui::Separator();
            DrawWaypointFields(newWaypoint_);
            ImGui::Spacing();
            if (ImGui::Button("Create Waypoint")) {
                if (SaveWaypoint(db, newWaypoint_)) {
                    FetchWaypoints(db, editingArea_.name);
                    needFetchWaypoints_ = false;
                    showNewWaypoint_    = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##nwp")) showNewWaypoint_ = false;

        } else if (selectedWaypoint_ >= 0 && selectedWaypoint_ < (int)waypoints_.size()) {
            ImGui::Spacing();
            ImGui::Text("Waypoint [id=%d]", editingWaypoint_.id);
            ImGui::Separator();
            if (DrawWaypointFields(editingWaypoint_)) dirtyWaypoint_ = true;
            ImGui::Spacing();

            ImGui::BeginDisabled(!dirtyWaypoint_);
            if (ImGui::Button("Save Waypoint")) {
                if (SaveWaypoint(db, editingWaypoint_)) {
                    FetchWaypoints(db, editingArea_.name);
                    needFetchWaypoints_ = false;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Waypoint")) {
                editingWaypoint_ = waypoints_[selectedWaypoint_];
                dirtyWaypoint_   = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
            if (ImGui::Button("Delete Waypoint")) {
                DeleteWaypoint(db, editingWaypoint_.id);
                FetchWaypoints(db, editingArea_.name);
                needFetchWaypoints_ = false;
            }
            ImGui::PopStyleColor();
        }

        // --- World Objects section ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("World Objects");
        ImGui::SameLine();
        if (ImGui::SmallButton("New Object")) {
            newWorldObject_           = {};
            newWorldObject_.area_name = editingArea_.name;
            newWorldObject_.scale     = 1.f;
            showNewWorldObject_       = true;
            selectedWorldObject_      = -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(static mesh instances — trees, rocks, buildings…)");
        ImGui::Separator();

        float woListH = std::min(120.f, 28.f * (float)(world_objects_.size() + 1));
        ImGui::BeginChild("##wo_list", {0, woListH}, true);
        for (int i = 0; i < (int)world_objects_.size(); ++i) {
            const auto& w = world_objects_[i];
            // Resolve model name for display
            std::string mname = "(none)";
            for (const auto& m : wo_models_)
                if (m.id == w.model_id) { mname = m.name; break; }
            char label[200];
            std::snprintf(label, sizeof(label), "[%d] %s (%.0f,%.0f,%.0f) yaw=%.0f s=%.2f##wol%d",
                          w.id, mname.c_str(), w.x, w.y, w.z, w.yaw, w.scale, i);
            if (ImGui::Selectable(label, selectedWorldObject_ == i)) {
                selectedWorldObject_  = i;
                editingWorldObject_   = w;
                dirtyWorldObject_     = false;
                showNewWorldObject_   = false;
            }
        }
        if (world_objects_.empty())
            ImGui::TextDisabled("No world objects in this area.");
        ImGui::EndChild();

        // Helper: draw model combo + transform fields
        auto drawWOFields = [&](AreaWorldObject& w, const char* idSuffix) -> bool {
            bool changed = false;
            // Model picker
            std::string curName = "(none)";
            int selIdx = -1;
            for (int i = 0; i < (int)wo_models_.size(); ++i)
                if (wo_models_[i].id == w.model_id) { curName = wo_models_[i].name; selIdx = i; break; }
            ImGui::SetNextItemWidth(-1.f);
            char comboId[32]; std::snprintf(comboId, sizeof(comboId), "Model##%s", idSuffix);
            if (ImGui::BeginCombo(comboId, curName.c_str())) {
                for (int i = 0; i < (int)wo_models_.size(); ++i) {
                    bool sel = (i == selIdx);
                    if (ImGui::Selectable(wo_models_[i].name.c_str(), sel)) {
                        w.model_id = wo_models_[i].id; changed = true;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            float w3 = (ImGui::GetContentRegionAvail().x - 8.f) / 3.f;
            char xId[32], yId[32], zId[32], yawId[32], scaleId[32];
            std::snprintf(xId,    sizeof(xId),    "X##%s",     idSuffix);
            std::snprintf(yId,    sizeof(yId),    "Y##%s",     idSuffix);
            std::snprintf(zId,    sizeof(zId),    "Z##%s",     idSuffix);
            std::snprintf(yawId,  sizeof(yawId),  "Yaw##%s",   idSuffix);
            std::snprintf(scaleId,sizeof(scaleId),"Scale##%s", idSuffix);
            ImGui::SetNextItemWidth(w3);
            if (ImGui::InputFloat(xId,  &w.x, 0.f, 0.f, "%.1f")) changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(w3);
            if (ImGui::InputFloat(yId,  &w.y, 0.f, 0.f, "%.1f")) changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputFloat(zId,  &w.z, 0.f, 0.f, "%.1f")) changed = true;
            float w2 = (ImGui::GetContentRegionAvail().x - 8.f) / 2.f;
            ImGui::SetNextItemWidth(w2);
            if (ImGui::InputFloat(yawId,  &w.yaw,   1.f, 10.f, "%.1f°")) changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputFloat(scaleId,&w.scale,  0.05f, 0.5f, "%.3f")) changed = true;
            if (w.scale < 0.01f) w.scale = 0.01f;
            return changed;
        };

        if (showNewWorldObject_) {
            ImGui::Spacing();
            ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New World Object");
            ImGui::Separator();
            drawWOFields(newWorldObject_, "nwo");
            ImGui::Spacing();
            if (ImGui::Button("Create Object")) {
                if (SaveWorldObject(db, newWorldObject_)) {
                    FetchWorldObjects(db, editingArea_.name);
                    needFetchWorldObjects_ = false;
                    showNewWorldObject_    = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##nwo")) showNewWorldObject_ = false;

        } else if (selectedWorldObject_ >= 0 && selectedWorldObject_ < (int)world_objects_.size()) {
            ImGui::Spacing();
            ImGui::Text("World Object [id=%d]", editingWorldObject_.id);
            ImGui::Separator();
            if (drawWOFields(editingWorldObject_, "ewo")) dirtyWorldObject_ = true;
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirtyWorldObject_);
            if (ImGui::Button("Save Object")) {
                if (SaveWorldObject(db, editingWorldObject_)) {
                    FetchWorldObjects(db, editingArea_.name);
                    needFetchWorldObjects_ = false;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Object")) {
                editingWorldObject_ = world_objects_[selectedWorldObject_];
                dirtyWorldObject_   = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
            if (ImGui::Button("Delete Object")) {
                DeleteWorldObject(db, editingWorldObject_.id);
                FetchWorldObjects(db, editingArea_.name);
                needFetchWorldObjects_ = false;
            }
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Changes take effect on next server restart.");

    } else {
        ImGui::TextDisabled("Select an area, or click \"New Area\".");
    }

    ImGui::EndChild();
}

} // namespace gue
