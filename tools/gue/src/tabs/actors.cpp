#include "actors.h"
#include "media.h"
#include <imgui.h>
#include <cstring>
#include <cstdio>

namespace gue {

static const char* kAggTypes[] = {
    "Passive (0)",
    "Defensive (1)",
    "Aggressive (2)",
    "Dialog-Only (3)",
};

// ---------------------------------------------------------------------------
// Table setup
// ---------------------------------------------------------------------------

void ActorsTab::EnsureTable(sqlite3* db) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS npc_spawns ("
        "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name              TEXT    NOT NULL DEFAULT 'NPC',"
        "  race              TEXT    NOT NULL DEFAULT 'Human',"
        "  class             TEXT    NOT NULL DEFAULT 'Warrior',"
        "  level             INTEGER NOT NULL DEFAULT 1,"
        "  area_name         TEXT    NOT NULL DEFAULT 'Starter Zone',"
        "  x                 REAL    NOT NULL DEFAULT 0,"
        "  y                 REAL    NOT NULL DEFAULT 0,"
        "  z                 REAL    NOT NULL DEFAULT 0,"
        "  yaw               REAL    NOT NULL DEFAULT 0,"
        "  aggressiveness    INTEGER NOT NULL DEFAULT 0,"
        "  aggressive_range  REAL    NOT NULL DEFAULT 8.0,"
        "  attack_range      REAL    NOT NULL DEFAULT 2.0,"
        "  respawn_delay_ms  INTEGER NOT NULL DEFAULT 30000,"
        "  actor_def_id      INTEGER NOT NULL DEFAULT 0,"
        "  start_waypoint_id INTEGER NOT NULL DEFAULT 0"
        ")";
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    // Backfill columns for pre-existing DBs.
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN actor_def_id INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN start_waypoint_id INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN wander_radius REAL NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN wander_pause_min_ms INTEGER NOT NULL DEFAULT 2000",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN wander_pause_max_ms INTEGER NOT NULL DEFAULT 5000",
        nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// DB helpers
// ---------------------------------------------------------------------------

void ActorsTab::Fetch(sqlite3* db) {
    EnsureTable(db);
    spawns_.clear();
    selected_ = -1;

    sqlite3_stmt* stmt = nullptr;
    // Also ensure the attack_range column exists for older DBs.
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN attack_range REAL NOT NULL DEFAULT 2.0",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN start_waypoint_id INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN wander_radius REAL NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN wander_pause_min_ms INTEGER NOT NULL DEFAULT 2000",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE npc_spawns ADD COLUMN wander_pause_max_ms INTEGER NOT NULL DEFAULT 5000",
        nullptr, nullptr, nullptr);

    const char* sql =
        "SELECT id, name, race, class, level, area_name, x, y, z, yaw,"
        "       aggressiveness, aggressive_range, attack_range, respawn_delay_ms,"
        "       actor_def_id, start_waypoint_id,"
        "       wander_radius, wander_pause_min_ms, wander_pause_max_ms"
        " FROM npc_spawns ORDER BY area_name, id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Fetch error: %s", sqlite3_errmsg(db));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NpcSpawn n;
        n.id               = sqlite3_column_int(stmt, 0);
        n.name             = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        n.race             = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        n.class_           = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        n.level            = sqlite3_column_int(stmt, 4);
        n.area_name        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        n.x                = static_cast<float>(sqlite3_column_double(stmt, 6));
        n.y                = static_cast<float>(sqlite3_column_double(stmt, 7));
        n.z                = static_cast<float>(sqlite3_column_double(stmt, 8));
        n.yaw              = static_cast<float>(sqlite3_column_double(stmt, 9));
        n.aggressiveness   = sqlite3_column_int(stmt, 10);
        n.aggressive_range = static_cast<float>(sqlite3_column_double(stmt, 11));
        n.attack_range     = static_cast<float>(sqlite3_column_double(stmt, 12));
        n.respawn_delay_ms  = sqlite3_column_int(stmt, 13);
        n.actor_def_id      = sqlite3_column_int(stmt, 14);
        n.start_waypoint_id = sqlite3_column_int(stmt, 15);
        n.wander_radius     = static_cast<float>(sqlite3_column_double(stmt, 16));
        n.wander_pause_min  = sqlite3_column_int(stmt, 17);
        n.wander_pause_max  = sqlite3_column_int(stmt, 18);
        spawns_.push_back(n);
    }
    sqlite3_finalize(stmt);
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Loaded %d NPC spawns.", (int)spawns_.size());
}

bool ActorsTab::Save(sqlite3* db, NpcSpawn& n) {
    sqlite3_stmt* stmt = nullptr;
    int rc;

    if (n.id == 0) {
        const char* sql =
            "INSERT INTO npc_spawns"
            " (name,race,class,level,area_name,x,y,z,yaw,"
            "  aggressiveness,aggressive_range,attack_range,respawn_delay_ms,"
            "  actor_def_id,start_waypoint_id,"
            "  wander_radius,wander_pause_min_ms,wander_pause_max_ms)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt,   1, n.name.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,   2, n.race.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,   3, n.class_.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,    4, n.level);
        sqlite3_bind_text(stmt,   5, n.area_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt,  6, n.x);
        sqlite3_bind_double(stmt,  7, n.y);
        sqlite3_bind_double(stmt,  8, n.z);
        sqlite3_bind_double(stmt,  9, n.yaw);
        sqlite3_bind_int(stmt,   10, n.aggressiveness);
        sqlite3_bind_double(stmt, 11, n.aggressive_range);
        sqlite3_bind_double(stmt, 12, n.attack_range);
        sqlite3_bind_int(stmt,   13, n.respawn_delay_ms);
        sqlite3_bind_int(stmt,   14, n.actor_def_id);
        sqlite3_bind_int(stmt,   15, n.start_waypoint_id);
        sqlite3_bind_double(stmt, 16, n.wander_radius);
        sqlite3_bind_int(stmt,   17, n.wander_pause_min);
        sqlite3_bind_int(stmt,   18, n.wander_pause_max);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
        n.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        const char* sql =
            "UPDATE npc_spawns SET"
            " name=?, race=?, class=?, level=?, area_name=?,"
            " x=?, y=?, z=?, yaw=?,"
            " aggressiveness=?, aggressive_range=?, attack_range=?, respawn_delay_ms=?,"
            " actor_def_id=?, start_waypoint_id=?,"
            " wander_radius=?, wander_pause_min_ms=?, wander_pause_max_ms=?"
            " WHERE id=?";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt,   1, n.name.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,   2, n.race.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,   3, n.class_.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,    4, n.level);
        sqlite3_bind_text(stmt,   5, n.area_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt,  6, n.x);
        sqlite3_bind_double(stmt,  7, n.y);
        sqlite3_bind_double(stmt,  8, n.z);
        sqlite3_bind_double(stmt,  9, n.yaw);
        sqlite3_bind_int(stmt,   10, n.aggressiveness);
        sqlite3_bind_double(stmt, 11, n.aggressive_range);
        sqlite3_bind_double(stmt, 12, n.attack_range);
        sqlite3_bind_int(stmt,   13, n.respawn_delay_ms);
        sqlite3_bind_int(stmt,   14, n.actor_def_id);
        sqlite3_bind_int(stmt,   15, n.start_waypoint_id);
        sqlite3_bind_double(stmt, 16, n.wander_radius);
        sqlite3_bind_int(stmt,   17, n.wander_pause_min);
        sqlite3_bind_int(stmt,   18, n.wander_pause_max);
        sqlite3_bind_int(stmt,   19, n.id);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
    }

    sqlite3_finalize(stmt);
    needFetch_ = true;
    dirty_     = false;
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Saved '%s' (id=%d).", n.name.c_str(), n.id);
    return true;

err:
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Save error: %s", sqlite3_errmsg(db));
    if (stmt) sqlite3_finalize(stmt);
    return false;
}

bool ActorsTab::Delete(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM npc_spawns WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    needFetch_ = true;
    selected_  = -1;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted spawn %d.", id);
    return true;
}

// ---------------------------------------------------------------------------
// DrawFields
// ---------------------------------------------------------------------------

static bool DrawFields(NpcSpawn& n, MediaTab* media) {
    bool changed = false;

    char buf[128];

    // Name
    std::strncpy(buf, n.name.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    if (ImGui::InputText("Name", buf, sizeof(buf))) { n.name = buf; changed = true; }

    // Actor Def picker (new Media system) — sourced from media_actor_defs.
    if (media) {
        const auto& defs = media->ActorDefs();
        const char* curName = "(none)";
        for (auto& d : defs) if (d.id == n.actor_def_id) { curName = d.name.c_str(); break; }
        if (ImGui::BeginCombo("Actor Def", curName)) {
            if (ImGui::Selectable("(none)", n.actor_def_id == 0)) {
                n.actor_def_id = 0; changed = true;
            }
            for (auto& d : defs) {
                bool sel = d.id == n.actor_def_id;
                if (ImGui::Selectable(d.name.c_str(), sel)) {
                    n.actor_def_id = d.id; changed = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Meshes, materials and animations come from this Media actor def.");
    }

    // Race / Class side by side (kept as free text for legacy / display purposes).
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f - 4);
    std::strncpy(buf, n.race.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    if (ImGui::InputText("Race", buf, sizeof(buf))) { n.race = buf; changed = true; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    std::strncpy(buf, n.class_.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    if (ImGui::InputText("Class", buf, sizeof(buf))) { n.class_ = buf; changed = true; }

    if (ImGui::InputInt("Level", &n.level)) { changed = true; }
    if (n.level < 1) n.level = 1;

    ImGui::Separator();
    ImGui::TextUnformatted("Spawn Location");

    std::strncpy(buf, n.area_name.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    if (ImGui::InputText("Area", buf, sizeof(buf))) { n.area_name = buf; changed = true; }

    float colW = (ImGui::GetContentRegionAvail().x - 16) / 4.f;
    ImGui::SetNextItemWidth(colW);
    if (ImGui::InputFloat("X##pos", &n.x, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(colW);
    if (ImGui::InputFloat("Y##pos", &n.y, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(colW);
    if (ImGui::InputFloat("Z##pos", &n.z, 0.f, 0.f, "%.2f")) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputFloat("Yaw", &n.yaw, 0.f, 0.f, "%.1f")) changed = true;
    if (n.yaw < 0.f)   n.yaw += 360.f;
    if (n.yaw >= 360.f) n.yaw -= 360.f;

    ImGui::Separator();
    ImGui::TextUnformatted("Behaviour");

    if (ImGui::Combo("Aggressiveness", &n.aggressiveness, kAggTypes, 4)) changed = true;

    if (n.aggressiveness == 2) {
        if (ImGui::InputFloat("Aggro Range", &n.aggressive_range, 0.5f, 2.f, "%.1f")) changed = true;
        if (n.aggressive_range < 0.f) n.aggressive_range = 0.f;
        ImGui::TextDisabled("Player enters this radius → NPC starts chasing.");
    }

    {
        float colW = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
        ImGui::SetNextItemWidth(colW);
        if (ImGui::InputFloat("Attack Range", &n.attack_range, 0.5f, 2.f, "%.1f")) changed = true;
        if (n.attack_range < 0.1f) n.attack_range = 0.1f;
        ImGui::SameLine();
        ImGui::TextDisabled("Melee ≈ 2  |  Ranged ≈ 15–25");
    }

    if (ImGui::InputInt("Respawn Delay (ms)", &n.respawn_delay_ms)) changed = true;
    if (n.respawn_delay_ms < 0) n.respawn_delay_ms = 0;
    ImGui::TextDisabled("0 = permanent death. 30000 = 30 sec.");

    ImGui::Separator();
    ImGui::TextUnformatted("Movement");

    if (ImGui::InputInt("Start Waypoint ID", &n.start_waypoint_id)) changed = true;
    if (n.start_waypoint_id < 0) n.start_waypoint_id = 0;
    ImGui::TextDisabled("Waypoint patrol: 0 = off. ID from Areas tab.");

    ImGui::Spacing();
    if (ImGui::InputFloat("Wander Radius", &n.wander_radius, 1.f, 5.f, "%.1f")) changed = true;
    if (n.wander_radius < 0.f) n.wander_radius = 0.f;
    ImGui::TextDisabled("Random walk: 0 = off. Max distance from spawn.");

    if (n.wander_radius > 0.f) {
        float hw = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
        ImGui::SetNextItemWidth(hw);
        if (ImGui::InputInt("Pause Min (ms)##w", &n.wander_pause_min)) changed = true;
        if (n.wander_pause_min < 0) n.wander_pause_min = 0;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("Pause Max (ms)##w", &n.wander_pause_max)) changed = true;
        if (n.wander_pause_max < n.wander_pause_min) n.wander_pause_max = n.wander_pause_min;
    }

    return changed;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void ActorsTab::Draw(sqlite3* db, MediaTab* media) {
    if (needFetch_) { Fetch(db); needFetch_ = false; }

    // Ensure the Media tab has loaded its data so our picker has entries to show,
    // even if the user never clicked the Media tab this session.
    if (media) {
        media->EnsureTables(db);
        if (media->ActorDefs().empty() && media->Models().empty())
            media->FetchAll(db);
    }

    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New NPC Spawn")) { newSpawn_ = {}; showNew_ = true; selected_ = -1; }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    // Left: grouped list
    float listW = 220.f;
    ImGui::BeginChild("##actor_list", {listW, 0}, true);
    std::string lastArea;
    for (int i = 0; i < (int)spawns_.size(); ++i) {
        auto& s = spawns_[i];
        if (s.area_name != lastArea) {
            ImGui::TextColored({0.6f, 0.9f, 1.f, 1.f}, "%s", s.area_name.c_str());
            lastArea = s.area_name;
        }
        const char* aggIcon =
            s.aggressiveness == 2 ? "[A]" :
            s.aggressiveness == 3 ? "[D]" : "[ ]";
        char label[160];
        std::snprintf(label, sizeof(label), "  %s Lv%d %s##al%d",
                      aggIcon, s.level, s.name.c_str(), i);
        if (ImGui::Selectable(label, selected_ == i)) {
            selected_ = i;
            editing_  = s;
            dirty_    = false;
            showNew_  = false;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##actor_edit", {0, 0}, true);

    if (showNew_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New NPC Spawn");
        ImGui::Separator();
        DrawFields(newSpawn_, media);
        ImGui::Spacing();
        if (ImGui::Button("Create")) {
            if (Save(db, newSpawn_)) showNew_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) showNew_ = false;

    } else if (selected_ >= 0 && selected_ < (int)spawns_.size()) {
        ImGui::Text("Editing: [id=%d]  %s  (%s)",
                    editing_.id, editing_.name.c_str(), editing_.area_name.c_str());
        ImGui::Separator();
        if (DrawFields(editing_, media)) dirty_ = true;
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Save"))   Save(db, editing_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) { editing_ = spawns_[selected_]; dirty_ = false; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) Delete(db, editing_.id);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextDisabled("Changes take effect on next server restart.");

    } else {
        ImGui::TextDisabled("Select an NPC spawn, or click \"New NPC Spawn\".");
    }

    ImGui::EndChild();
}

} // namespace gue
