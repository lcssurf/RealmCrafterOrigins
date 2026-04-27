#include "spawn_points.h"
#include "media.h"

#include <imgui.h>
#include <cstring>
#include <cstdio>

namespace gue {

// ---------------------------------------------------------------------------
// DB helpers
// ---------------------------------------------------------------------------

void SpawnPointsTab::EnsureTables(sqlite3* db) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS spawn_points ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL DEFAULT 'Spawn Point',"
        "  area_name TEXT NOT NULL DEFAULT '',"
        "  x REAL NOT NULL DEFAULT 0,"
        "  y REAL NOT NULL DEFAULT 0,"
        "  z REAL NOT NULL DEFAULT 0,"
        "  radius REAL NOT NULL DEFAULT 5"
        ");"
        "CREATE TABLE IF NOT EXISTS spawn_point_mobs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  spawn_point_id INTEGER NOT NULL DEFAULT 0,"
        "  actor_def_id INTEGER NOT NULL DEFAULT 0,"
        "  mob_count INTEGER NOT NULL DEFAULT 1,"
        "  name TEXT NOT NULL DEFAULT 'NPC',"
        "  race TEXT NOT NULL DEFAULT 'Human',"
        "  class TEXT NOT NULL DEFAULT 'Warrior',"
        "  level INTEGER NOT NULL DEFAULT 1,"
        "  aggressiveness INTEGER NOT NULL DEFAULT 2,"
        "  aggressive_range REAL NOT NULL DEFAULT 8.0,"
        "  attack_range REAL NOT NULL DEFAULT 2.0,"
        "  respawn_delay_ms INTEGER NOT NULL DEFAULT 30000"
        ");";
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

void SpawnPointsTab::FetchAll(sqlite3* db) {
    points_.clear();
    sel_point_ = -1;
    sel_mob_   = -1;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, area_name, x, y, z, radius FROM spawn_points ORDER BY id",
        -1, &st, nullptr) != SQLITE_OK) return;

    while (sqlite3_step(st) == SQLITE_ROW) {
        SpawnPoint sp{};
        sp.id = sqlite3_column_int(st, 0);
        const char* n = (const char*)sqlite3_column_text(st, 1);
        const char* a = (const char*)sqlite3_column_text(st, 2);
        if (n) { strncpy(sp.name,      n, sizeof(sp.name)-1);      sp.name[sizeof(sp.name)-1]           = 0; }
        if (a) { strncpy(sp.area_name, a, sizeof(sp.area_name)-1); sp.area_name[sizeof(sp.area_name)-1] = 0; }
        sp.x      = (float)sqlite3_column_double(st, 3);
        sp.y      = (float)sqlite3_column_double(st, 4);
        sp.z      = (float)sqlite3_column_double(st, 5);
        sp.radius = (float)sqlite3_column_double(st, 6);
        points_.push_back(sp);
    }
    sqlite3_finalize(st);

    // Load mobs for each point
    for (auto& sp : points_) {
        sqlite3_stmt* ms = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT id, spawn_point_id, actor_def_id, mob_count, name, race, class,"
            " level, aggressiveness, aggressive_range, attack_range, respawn_delay_ms"
            " FROM spawn_point_mobs WHERE spawn_point_id=? ORDER BY id",
            -1, &ms, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_int(ms, 1, sp.id);
        while (sqlite3_step(ms) == SQLITE_ROW) {
            SpawnPointMob m{};
            m.id             = sqlite3_column_int(ms, 0);
            m.spawn_point_id = sqlite3_column_int(ms, 1);
            m.actor_def_id   = sqlite3_column_int(ms, 2);
            m.count          = sqlite3_column_int(ms, 3);
            const char* mn  = (const char*)sqlite3_column_text(ms, 4);
            const char* mr  = (const char*)sqlite3_column_text(ms, 5);
            const char* mc  = (const char*)sqlite3_column_text(ms, 6);
            if (mn) { strncpy(m.name,   mn, sizeof(m.name)-1);   m.name[sizeof(m.name)-1]   = 0; }
            if (mr) { strncpy(m.race,   mr, sizeof(m.race)-1);   m.race[sizeof(m.race)-1]   = 0; }
            if (mc) { strncpy(m.class_, mc, sizeof(m.class_)-1); m.class_[sizeof(m.class_)-1] = 0; }
            m.level            = sqlite3_column_int(ms, 7);
            m.aggressiveness   = sqlite3_column_int(ms, 8);
            m.aggressive_range = (float)sqlite3_column_double(ms, 9);
            m.attack_range     = (float)sqlite3_column_double(ms, 10);
            m.respawn_delay_ms = sqlite3_column_int(ms, 11);
            sp.mobs.push_back(m);
        }
        sqlite3_finalize(ms);
    }
}

bool SpawnPointsTab::SavePoint(sqlite3* db, SpawnPoint& sp) {
    if (sp.id == 0) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO spawn_points (name, area_name, x, y, z, radius)"
            " VALUES (?,?,?,?,?,?)",
            -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(st, 1, sp.name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, sp.area_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 3, sp.x);
        sqlite3_bind_double(st, 4, sp.y);
        sqlite3_bind_double(st, 5, sp.z);
        sqlite3_bind_double(st, 6, sp.radius);
        sqlite3_step(st);
        sqlite3_finalize(st);
        sp.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "UPDATE spawn_points SET name=?, area_name=?, x=?, y=?, z=?, radius=?"
            " WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(st, 1, sp.name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, sp.area_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 3, sp.x);
        sqlite3_bind_double(st, 4, sp.y);
        sqlite3_bind_double(st, 5, sp.z);
        sqlite3_bind_double(st, 6, sp.radius);
        sqlite3_bind_int(st, 7, sp.id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return true;
}

bool SpawnPointsTab::SaveMob(sqlite3* db, SpawnPointMob& m) {
    if (m.id == 0) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO spawn_point_mobs"
            " (spawn_point_id, actor_def_id, mob_count, name, race, class,"
            "  level, aggressiveness, aggressive_range, attack_range, respawn_delay_ms)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, m.spawn_point_id);
        sqlite3_bind_int(st, 2, m.actor_def_id);
        sqlite3_bind_int(st, 3, m.count);
        sqlite3_bind_text(st, 4, m.name,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, m.race,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, m.class_, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 7, m.level);
        sqlite3_bind_int(st, 8, m.aggressiveness);
        sqlite3_bind_double(st, 9, m.aggressive_range);
        sqlite3_bind_double(st, 10, m.attack_range);
        sqlite3_bind_int(st, 11, m.respawn_delay_ms);
        sqlite3_step(st);
        sqlite3_finalize(st);
        m.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "UPDATE spawn_point_mobs SET"
            " actor_def_id=?, mob_count=?, name=?, race=?, class=?,"
            " level=?, aggressiveness=?, aggressive_range=?, attack_range=?, respawn_delay_ms=?"
            " WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, m.actor_def_id);
        sqlite3_bind_int(st, 2, m.count);
        sqlite3_bind_text(st, 3, m.name,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, m.race,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, m.class_, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 6, m.level);
        sqlite3_bind_int(st, 7, m.aggressiveness);
        sqlite3_bind_double(st, 8, m.aggressive_range);
        sqlite3_bind_double(st, 9, m.attack_range);
        sqlite3_bind_int(st, 10, m.respawn_delay_ms);
        sqlite3_bind_int(st, 11, m.id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return true;
}

bool SpawnPointsTab::DeletePoint(sqlite3* db, int id) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM spawn_point_mobs WHERE spawn_point_id=%d", id);
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    snprintf(sql, sizeof(sql), "DELETE FROM spawn_points WHERE id=%d", id);
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool SpawnPointsTab::DeleteMob(sqlite3* db, int id) {
    char sql[64];
    snprintf(sql, sizeof(sql), "DELETE FROM spawn_point_mobs WHERE id=%d", id);
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void SpawnPointsTab::Draw(sqlite3* db, MediaTab* media) {
    EnsureTables(db);
    if (needFetch_) { FetchAll(db); needFetch_ = false; }

    if (statusMsg_[0]) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "%s", statusMsg_);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) statusMsg_[0] = 0;
    }

    // ----------------------------------------------------------------
    // Left: spawn point list
    // ----------------------------------------------------------------
    ImGui::BeginGroup();
    ImGui::Text("Spawn Points (%d)", (int)points_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("+ New")) {
        SpawnPoint np{};
        strcpy(np.name, "New Spawn Point");
        strcpy(np.area_name, "Starter Zone");
        if (SavePoint(db, np)) {
            points_.push_back(np);
            sel_point_ = (int)points_.size() - 1;
            sel_mob_   = -1;
            snprintf(statusMsg_, sizeof(statusMsg_), "Created spawn point id=%d", np.id);
        }
    }
    ImGui::Separator();
    ImGui::BeginChild("##sp_list", {220, 0}, true);
    for (int i = 0; i < (int)points_.size(); ++i) {
        auto& sp = points_[i];
        char label[200];
        snprintf(label, sizeof(label), "[%d] %s\n    %s (%.0f,%.0f,%.0f) r=%.1f  mobs=%d##sp%d",
            sp.id, sp.name, sp.area_name, sp.x, sp.y, sp.z, sp.radius, (int)sp.mobs.size(), i);
        if (ImGui::Selectable(label, sel_point_ == i)) {
            sel_point_ = i;
            sel_mob_   = -1;
        }
    }
    ImGui::EndChild();
    ImGui::EndGroup();

    if (sel_point_ < 0 || sel_point_ >= (int)points_.size()) return;

    ImGui::SameLine();

    // ----------------------------------------------------------------
    // Right: selected spawn point editor + mob list
    // ----------------------------------------------------------------
    ImGui::BeginGroup();
    auto& sp = points_[sel_point_];

    // --- Spawn point fields ---
    ImGui::SeparatorText("Spawn Point");
    bool dirty = false;
    dirty |= ImGui::InputText("Name##sp",      sp.name,      sizeof(sp.name));
    dirty |= ImGui::InputText("Area##sp",      sp.area_name, sizeof(sp.area_name));
    dirty |= ImGui::InputFloat("X##sp",  &sp.x, 1.f, 10.f, "%.2f");
    dirty |= ImGui::InputFloat("Y##sp",  &sp.y, 0.5f, 5.f, "%.2f");
    dirty |= ImGui::InputFloat("Z##sp",  &sp.z, 1.f, 10.f, "%.2f");
    dirty |= ImGui::SliderFloat("Radius##sp", &sp.radius, 0.5f, 50.f, "%.1f");
    if (dirty) SavePoint(db, sp);

    if (ImGui::Button("Delete Spawn Point")) {
        DeletePoint(db, sp.id);
        needFetch_ = true;
        ImGui::EndGroup();
        return;
    }

    // --- Mob list ---
    ImGui::SeparatorText("Mobs");
    if (ImGui::Button("+ Add Mob")) {
        SpawnPointMob nm{};
        nm.spawn_point_id = sp.id;
        strcpy(nm.name, "NPC");
        strcpy(nm.race, "Human");
        strcpy(nm.class_, "Warrior");
        nm.aggressiveness = 2;
        SaveMob(db, nm);
        sp.mobs.push_back(nm);
        sel_mob_ = (int)sp.mobs.size() - 1;
    }

    // Compact mob list
    ImGui::BeginChild("##mob_list", {0, 140}, true);
    for (int mi = 0; mi < (int)sp.mobs.size(); ++mi) {
        auto& m = sp.mobs[mi];
        // Resolve actor def name for display
        const char* defName = "(none)";
        if (media) {
            for (auto& d : media->ActorDefs())
                if (d.id == m.actor_def_id) { defName = d.name.c_str(); break; }
        }
        char label[200];
        snprintf(label, sizeof(label), "[%d] %s x%d  lv%d  def:%s##mob%d",
            m.id, m.name, m.count, m.level, defName, mi);
        if (ImGui::Selectable(label, sel_mob_ == mi))
            sel_mob_ = mi;
    }
    ImGui::EndChild();

    // --- Selected mob editor ---
    if (sel_mob_ >= 0 && sel_mob_ < (int)sp.mobs.size()) {
        auto& m = sp.mobs[sel_mob_];
        ImGui::SeparatorText("Edit Mob");
        bool mdirty = false;
        mdirty |= ImGui::InputText("Name##mob",  m.name,   sizeof(m.name));
        mdirty |= ImGui::InputText("Race##mob",  m.race,   sizeof(m.race));
        mdirty |= ImGui::InputText("Class##mob", m.class_, sizeof(m.class_));
        mdirty |= ImGui::InputInt("Level##mob",  &m.level);
        mdirty |= ImGui::InputInt("Count##mob",  &m.count);
        if (m.count  < 1)  m.count  = 1;
        if (m.level  < 1)  m.level  = 1;

        // Actor Def picker
        if (media) {
            auto& defs = media->ActorDefs();
            int cur_idx = -1;
            for (int di = 0; di < (int)defs.size(); ++di)
                if (defs[di].id == m.actor_def_id) { cur_idx = di; break; }
            const char* preview = (cur_idx >= 0) ? defs[cur_idx].name.c_str() : "(none)";
            if (ImGui::BeginCombo("Actor Def##mob", preview)) {
                if (ImGui::Selectable("(none)", m.actor_def_id == 0)) {
                    m.actor_def_id = 0; mdirty = true;
                }
                for (int di = 0; di < (int)defs.size(); ++di) {
                    bool sel = (defs[di].id == m.actor_def_id);
                    if (ImGui::Selectable(defs[di].name.c_str(), sel)) {
                        m.actor_def_id = defs[di].id; mdirty = true;
                    }
                }
                ImGui::EndCombo();
            }
        }

        static const char* kAggLabels[] = {"Passive","Defensive","Aggressive","Dialog-only"};
        mdirty |= ImGui::Combo("Aggro##mob", &m.aggressiveness, kAggLabels, 4);
        mdirty |= ImGui::InputFloat("Aggro Range##mob",  &m.aggressive_range, 0.5f, 5.f, "%.1f");
        mdirty |= ImGui::InputFloat("Attack Range##mob", &m.attack_range,     0.5f, 5.f, "%.1f");
        mdirty |= ImGui::InputInt("Respawn (ms)##mob",   &m.respawn_delay_ms);
        if (m.respawn_delay_ms < 0) m.respawn_delay_ms = 0;

        if (mdirty) SaveMob(db, m);

        ImGui::Spacing();
        if (ImGui::Button("Remove Mob")) {
            DeleteMob(db, m.id);
            sp.mobs.erase(sp.mobs.begin() + sel_mob_);
            sel_mob_ = -1;
        }
    }

    ImGui::EndGroup();
}

} // namespace gue
