#include "media.h"
#include "../file_import.h"

#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace gue {

// ---------------------------------------------------------------------------
// Slot names
// ---------------------------------------------------------------------------

static const char* kSlotNames[] = {
    "Body",
    "Hair",
    "Helm",
    "Chest",
    "Hands",
    "Belt",
    "Legs",
    "Feet",
    "Weapon",
    "Shield",
    "Attachment",
};
static constexpr int kSlotCount = (int)(sizeof(kSlotNames) / sizeof(kSlotNames[0]));

const char* ActorSlotName(int slot) {
    if (slot < 0 || slot >= kSlotCount) return "?";
    return kSlotNames[slot];
}

// ---------------------------------------------------------------------------
// Table setup
// ---------------------------------------------------------------------------

void MediaTab::EnsureTables(sqlite3* db) {
    const char* sql[] = {
        "CREATE TABLE IF NOT EXISTS media_models ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name      TEXT    NOT NULL DEFAULT 'New Model',"
        "  file_path TEXT    NOT NULL DEFAULT '',"
        "  scale     REAL    NOT NULL DEFAULT 1.0"
        ")",

        "CREATE TABLE IF NOT EXISTS media_materials ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name        TEXT    NOT NULL DEFAULT 'New Material',"
        "  albedo_path TEXT    NOT NULL DEFAULT '',"
        "  normal_path TEXT    NOT NULL DEFAULT '',"
        "  orm_path    TEXT    NOT NULL DEFAULT '',"
        "  albedo_r    REAL    NOT NULL DEFAULT 0.72,"
        "  albedo_g    REAL    NOT NULL DEFAULT 0.68,"
        "  albedo_b    REAL    NOT NULL DEFAULT 0.60,"
        "  roughness   REAL    NOT NULL DEFAULT 0.5,"
        "  metallic    REAL    NOT NULL DEFAULT 0.0"
        ")",

        "CREATE TABLE IF NOT EXISTS media_anim_clips ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name          TEXT    NOT NULL DEFAULT 'New Clip',"
        "  source_path   TEXT    NOT NULL DEFAULT '',"
        "  clip_override TEXT    NOT NULL DEFAULT ''"
        ")",

        "CREATE TABLE IF NOT EXISTS media_actor_defs ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT    NOT NULL DEFAULT 'New Actor'"
        ")",

        "CREATE TABLE IF NOT EXISTS media_actor_meshes ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  actor_def_id INTEGER NOT NULL,"
        "  slot         INTEGER NOT NULL DEFAULT 0,"
        "  model_id     INTEGER NOT NULL DEFAULT 0,"
        "  material_id  INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY(actor_def_id) REFERENCES media_actor_defs(id)"
        ")",

        "CREATE TABLE IF NOT EXISTS media_actor_anims ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  actor_def_id INTEGER NOT NULL,"
        "  action       TEXT    NOT NULL DEFAULT 'Idle',"
        "  clip_id      INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY(actor_def_id) REFERENCES media_actor_defs(id)"
        ")",
    };
    for (const char* s : sql)
        sqlite3_exec(db, s, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Fetch all tables
// ---------------------------------------------------------------------------

static std::string colText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? std::string(t) : std::string();
}

void MediaTab::FetchAll(sqlite3* db) {
    EnsureTables(db);

    models_.clear();
    materials_.clear();
    clips_.clear();
    actor_defs_.clear();

    sqlite3_stmt* stmt = nullptr;

    // Models
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, file_path, scale FROM media_models ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MediaModel m;
            m.id        = sqlite3_column_int   (stmt, 0);
            m.name      = colText              (stmt, 1);
            m.file_path = colText              (stmt, 2);
            m.scale     = (float)sqlite3_column_double(stmt, 3);
            models_.push_back(std::move(m));
        }
        sqlite3_finalize(stmt);
    }

    // Materials
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, albedo_path, normal_path, orm_path,"
        "       albedo_r, albedo_g, albedo_b, roughness, metallic"
        " FROM media_materials ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MediaMaterial m;
            m.id          = sqlite3_column_int(stmt, 0);
            m.name        = colText(stmt, 1);
            m.albedo_path = colText(stmt, 2);
            m.normal_path = colText(stmt, 3);
            m.orm_path    = colText(stmt, 4);
            m.albedo_r    = (float)sqlite3_column_double(stmt, 5);
            m.albedo_g    = (float)sqlite3_column_double(stmt, 6);
            m.albedo_b    = (float)sqlite3_column_double(stmt, 7);
            m.roughness   = (float)sqlite3_column_double(stmt, 8);
            m.metallic    = (float)sqlite3_column_double(stmt, 9);
            materials_.push_back(std::move(m));
        }
        sqlite3_finalize(stmt);
    }

    // Anim Clips
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, source_path, clip_override"
        " FROM media_anim_clips ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MediaAnimClip c;
            c.id            = sqlite3_column_int(stmt, 0);
            c.name          = colText(stmt, 1);
            c.source_path   = colText(stmt, 2);
            c.clip_override = colText(stmt, 3);
            clips_.push_back(std::move(c));
        }
        sqlite3_finalize(stmt);
    }

    // Actor Defs
    if (sqlite3_prepare_v2(db,
        "SELECT id, name FROM media_actor_defs ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ActorDef d;
            d.id   = sqlite3_column_int(stmt, 0);
            d.name = colText(stmt, 1);
            actor_defs_.push_back(std::move(d));
        }
        sqlite3_finalize(stmt);
    }

    // Actor mesh slots
    if (sqlite3_prepare_v2(db,
        "SELECT id, actor_def_id, slot, model_id, material_id"
        " FROM media_actor_meshes ORDER BY actor_def_id, slot",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ActorMeshSlot s;
            s.id           = sqlite3_column_int(stmt, 0);
            s.actor_def_id = sqlite3_column_int(stmt, 1);
            s.slot         = sqlite3_column_int(stmt, 2);
            s.model_id     = sqlite3_column_int(stmt, 3);
            s.material_id  = sqlite3_column_int(stmt, 4);
            for (auto& d : actor_defs_) {
                if (d.id == s.actor_def_id) { d.mesh_slots.push_back(s); break; }
            }
        }
        sqlite3_finalize(stmt);
    }

    // Actor anim map
    if (sqlite3_prepare_v2(db,
        "SELECT id, actor_def_id, action, clip_id"
        " FROM media_actor_anims ORDER BY actor_def_id, action",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ActorAnimMap a;
            a.id           = sqlite3_column_int(stmt, 0);
            a.actor_def_id = sqlite3_column_int(stmt, 1);
            a.action       = colText(stmt, 2);
            a.clip_id      = sqlite3_column_int(stmt, 3);
            for (auto& d : actor_defs_) {
                if (d.id == a.actor_def_id) { d.anim_map.push_back(a); break; }
            }
        }
        sqlite3_finalize(stmt);
    }

    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Loaded: %d models, %d materials, %d clips, %d actor defs",
                  (int)models_.size(), (int)materials_.size(),
                  (int)clips_.size(),  (int)actor_defs_.size());
}

// ---------------------------------------------------------------------------
// CRUD — Models
// ---------------------------------------------------------------------------

void MediaTab::SaveModel(sqlite3* db, MediaModel& m) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (m.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_models (name, file_path, scale) VALUES (?, ?, ?)",
            -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_models SET name=?, file_path=?, scale=? WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save model error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text  (stmt, 1, m.name.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 2, m.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, m.scale);
    if (m.id != 0) sqlite3_bind_int(stmt, 4, m.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save model error: %s", sqlite3_errmsg(db));
    } else {
        if (m.id == 0) m.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Saved model '%s' (id=%d).", m.name.c_str(), m.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteModel(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_models WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
    selModel_  = -1;
}

// ---------------------------------------------------------------------------
// CRUD — Materials
// ---------------------------------------------------------------------------

void MediaTab::SaveMaterial(sqlite3* db, MediaMaterial& m) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (m.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_materials"
            " (name, albedo_path, normal_path, orm_path,"
            "  albedo_r, albedo_g, albedo_b, roughness, metallic)"
            " VALUES (?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_materials SET"
            " name=?, albedo_path=?, normal_path=?, orm_path=?,"
            " albedo_r=?, albedo_g=?, albedo_b=?, roughness=?, metallic=?"
            " WHERE id=?", -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save material error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text  (stmt, 1, m.name.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 2, m.albedo_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 3, m.normal_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 4, m.orm_path.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, m.albedo_r);
    sqlite3_bind_double(stmt, 6, m.albedo_g);
    sqlite3_bind_double(stmt, 7, m.albedo_b);
    sqlite3_bind_double(stmt, 8, m.roughness);
    sqlite3_bind_double(stmt, 9, m.metallic);
    if (m.id != 0) sqlite3_bind_int(stmt, 10, m.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save material error: %s", sqlite3_errmsg(db));
    } else {
        if (m.id == 0) m.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Saved material '%s' (id=%d).", m.name.c_str(), m.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteMaterial(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_materials WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
    selMat_    = -1;
}

// ---------------------------------------------------------------------------
// CRUD — Anim Clips
// ---------------------------------------------------------------------------

void MediaTab::SaveAnimClip(sqlite3* db, MediaAnimClip& c) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (c.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_anim_clips (name, source_path, clip_override)"
            " VALUES (?, ?, ?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_anim_clips SET name=?, source_path=?, clip_override=? WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save clip error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, c.name.c_str(),          -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, c.source_path.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, c.clip_override.c_str(), -1, SQLITE_TRANSIENT);
    if (c.id != 0) sqlite3_bind_int(stmt, 4, c.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save clip error: %s", sqlite3_errmsg(db));
    } else {
        if (c.id == 0) c.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Saved clip '%s' (id=%d).", c.name.c_str(), c.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteAnimClip(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_anim_clips WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
    selClip_   = -1;
}

// ---------------------------------------------------------------------------
// CRUD — Actor Defs + children
// ---------------------------------------------------------------------------

void MediaTab::SaveActorDef(sqlite3* db, ActorDef& d) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (d.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_actor_defs (name) VALUES (?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_actor_defs SET name=? WHERE id=?", -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save actor def error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, d.name.c_str(), -1, SQLITE_TRANSIENT);
    if (d.id != 0) sqlite3_bind_int(stmt, 2, d.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save actor def error: %s", sqlite3_errmsg(db));
    } else {
        if (d.id == 0) d.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Saved actor def '%s' (id=%d).", d.name.c_str(), d.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteActorDef(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

    sqlite3_prepare_v2(db, "DELETE FROM media_actor_meshes WHERE actor_def_id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "DELETE FROM media_actor_anims WHERE actor_def_id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "DELETE FROM media_actor_defs WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    needFetch_   = true;
    selActorDef_ = -1;
}

void MediaTab::SaveMeshSlot(sqlite3* db, ActorMeshSlot& s) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (s.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_actor_meshes (actor_def_id, slot, model_id, material_id)"
            " VALUES (?, ?, ?, ?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_actor_meshes SET slot=?, model_id=?, material_id=? WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) return;

    if (s.id == 0) {
        sqlite3_bind_int(stmt, 1, s.actor_def_id);
        sqlite3_bind_int(stmt, 2, s.slot);
        sqlite3_bind_int(stmt, 3, s.model_id);
        sqlite3_bind_int(stmt, 4, s.material_id);
    } else {
        sqlite3_bind_int(stmt, 1, s.slot);
        sqlite3_bind_int(stmt, 2, s.model_id);
        sqlite3_bind_int(stmt, 3, s.material_id);
        sqlite3_bind_int(stmt, 4, s.id);
    }
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        if (s.id == 0) s.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteMeshSlot(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_actor_meshes WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
}

void MediaTab::SaveAnimMap(sqlite3* db, ActorAnimMap& a) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (a.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_actor_anims (actor_def_id, action, clip_id)"
            " VALUES (?, ?, ?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_actor_anims SET action=?, clip_id=? WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) return;

    if (a.id == 0) {
        sqlite3_bind_int (stmt, 1, a.actor_def_id);
        sqlite3_bind_text(stmt, 2, a.action.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 3, a.clip_id);
    } else {
        sqlite3_bind_text(stmt, 1, a.action.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 2, a.clip_id);
        sqlite3_bind_int (stmt, 3, a.id);
    }
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        if (a.id == 0) a.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteAnimMap(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_actor_anims WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
}

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------

static bool InputString(const char* id, std::string& s, size_t maxLen = 256) {
    std::vector<char> buf(maxLen);
    std::strncpy(buf.data(), s.c_str(), maxLen - 1);
    buf[maxLen - 1] = 0;
    if (ImGui::InputText(id, buf.data(), maxLen)) {
        s = buf.data();
        return true;
    }
    return false;
}

// Path input field with a native "Browse..." button. Picking a file outside
// of dist/client/assets/ copies it into assets/<target_subdir>/; already-in-tree
// files are referenced directly. Returns true if the path changed.
static bool PathField(const char* label, std::string& path,
                      const char* filter_label,
                      const char* filter_exts,
                      const char* target_subdir) {
    bool changed = false;
    // Reserve space on the right for the button.
    float btnW = 36.f;
    float avail = ImGui::GetContentRegionAvail().x;
    // Use a reasonable width for the text box, leaving room for button + label.
    ImGui::SetNextItemWidth(avail - btnW - 140.f);
    if (InputString(label, path, 512)) changed = true;
    ImGui::SameLine(0.f, 4.f);
    std::string btnId = std::string("...##br_") + label;
    if (ImGui::Button(btnId.c_str(), {btnW, 0})) {
        std::string picked = gue::PickAndImportAsset(filter_label, filter_exts, target_subdir);
        if (!picked.empty()) {
            path = picked;
            changed = true;
        }
    }
    return changed;
}

// Combo for picking from a list of (id, name) pairs; value is the id.
// Pass -1 as currentId to mean "none/unset" (selects first blank item).
static bool ComboId(const char* label, int& currentId,
                    const std::vector<std::pair<int, std::string>>& items,
                    const char* emptyLabel = "(none)") {
    int curIdx = 0; // 0 = "(none)"
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].first == currentId) { curIdx = (int)(i + 1); break; }
    }

    bool changed = false;
    const std::string& curLabel =
        curIdx == 0 ? std::string(emptyLabel) : items[curIdx - 1].second;

    if (ImGui::BeginCombo(label, curLabel.c_str())) {
        if (ImGui::Selectable(emptyLabel, curIdx == 0)) {
            currentId = 0;
            changed = true;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            bool sel = curIdx == (int)(i + 1);
            if (ImGui::Selectable(items[i].second.c_str(), sel)) {
                currentId = items[i].first;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static std::vector<std::pair<int, std::string>>
    idNameList(const std::vector<MediaModel>& v) {
    std::vector<std::pair<int, std::string>> out;
    for (auto& m : v) out.push_back({m.id, m.name});
    return out;
}
static std::vector<std::pair<int, std::string>>
    idNameList(const std::vector<MediaMaterial>& v) {
    std::vector<std::pair<int, std::string>> out;
    for (auto& m : v) out.push_back({m.id, m.name});
    return out;
}
static std::vector<std::pair<int, std::string>>
    idNameList(const std::vector<MediaAnimClip>& v) {
    std::vector<std::pair<int, std::string>> out;
    for (auto& c : v) out.push_back({c.id, c.name});
    return out;
}

// ---------------------------------------------------------------------------
// Models sub-tab
// ---------------------------------------------------------------------------

void MediaTab::DrawModels(sqlite3* db) {
    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Model")) {
        pendingModel_ = {};
        newModel_ = true;
        selModel_ = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    ImGui::BeginChild("##mdl_list", {260, 0}, true);
    for (int i = 0; i < (int)models_.size(); ++i) {
        char label[256];
        std::snprintf(label, sizeof(label), "%s##ml%d", models_[i].name.c_str(), i);
        if (ImGui::Selectable(label, selModel_ == i)) {
            selModel_   = i;
            editModel_  = models_[i];
            dirtyModel_ = false;
            newModel_   = false;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##mdl_edit", {0, 0}, true);
    if (newModel_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Model");
        ImGui::Separator();
        InputString("Name", pendingModel_.name);
        PathField("File Path", pendingModel_.file_path,
                  "3D Model", "glb,fbx,obj,dae,b3d,gltf", "models");
        ImGui::InputFloat("Scale", &pendingModel_.scale, 0.1f, 1.f, "%.2f");
        ImGui::TextDisabled("Click [...] to pick a file. If it's outside dist/client/assets/,");
        ImGui::TextDisabled("it's copied into assets/models/ automatically.");
        ImGui::Spacing();
        if (ImGui::Button("Create")) { SaveModel(db, pendingModel_); newModel_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) newModel_ = false;
    } else if (selModel_ >= 0 && selModel_ < (int)models_.size()) {
        ImGui::Text("Editing: [id=%d] %s", editModel_.id, editModel_.name.c_str());
        ImGui::Separator();
        if (InputString("Name", editModel_.name)) dirtyModel_ = true;
        if (PathField("File Path", editModel_.file_path,
                      "3D Model", "glb,fbx,obj,dae,b3d,gltf", "models"))
            dirtyModel_ = true;
        if (ImGui::InputFloat("Scale", &editModel_.scale, 0.1f, 1.f, "%.2f"))
            dirtyModel_ = true;
        ImGui::TextDisabled("[...] imports automatically into assets/models/ if needed.");
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirtyModel_);
        if (ImGui::Button("Save")) { SaveModel(db, editModel_); dirtyModel_ = false; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) { editModel_ = models_[selModel_]; dirtyModel_ = false; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) DeleteModel(db, editModel_.id);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Select a model, or click \"New Model\".");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Materials sub-tab
// ---------------------------------------------------------------------------

static bool DrawMaterialFields(MediaMaterial& m) {
    bool changed = false;
    if (InputString("Name", m.name)) changed = true;
    if (PathField("Albedo", m.albedo_path, "Texture", "png,jpg,jpeg,tga,bmp", "textures"))
        changed = true;
    if (PathField("Normal", m.normal_path, "Texture", "png,jpg,jpeg,tga,bmp", "textures"))
        changed = true;
    if (PathField("ORM",    m.orm_path,    "Texture", "png,jpg,jpeg,tga,bmp", "textures"))
        changed = true;

    ImGui::Separator();
    ImGui::TextUnformatted("PBR Factors");
    float col[3] = {m.albedo_r, m.albedo_g, m.albedo_b};
    if (ImGui::ColorEdit3("Albedo Tint", col)) {
        m.albedo_r = col[0]; m.albedo_g = col[1]; m.albedo_b = col[2];
        changed = true;
    }
    if (ImGui::SliderFloat("Roughness", &m.roughness, 0.f, 1.f)) changed = true;
    if (ImGui::SliderFloat("Metallic",  &m.metallic,  0.f, 1.f)) changed = true;

    ImGui::TextDisabled("ORM = Occlusion(R) / Roughness(G) / Metallic(B) packed texture.");
    return changed;
}

void MediaTab::DrawMaterials(sqlite3* db) {
    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Material")) {
        pendingMat_ = {};
        newMat_  = true;
        selMat_  = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    ImGui::BeginChild("##mat_list", {260, 0}, true);
    for (int i = 0; i < (int)materials_.size(); ++i) {
        char label[256];
        std::snprintf(label, sizeof(label), "%s##mml%d", materials_[i].name.c_str(), i);
        if (ImGui::Selectable(label, selMat_ == i)) {
            selMat_   = i;
            editMat_  = materials_[i];
            dirtyMat_ = false;
            newMat_   = false;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##mat_edit", {0, 0}, true);
    if (newMat_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Material");
        ImGui::Separator();
        DrawMaterialFields(pendingMat_);
        ImGui::Spacing();
        if (ImGui::Button("Create")) { SaveMaterial(db, pendingMat_); newMat_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) newMat_ = false;
    } else if (selMat_ >= 0 && selMat_ < (int)materials_.size()) {
        ImGui::Text("Editing: [id=%d] %s", editMat_.id, editMat_.name.c_str());
        ImGui::Separator();
        if (DrawMaterialFields(editMat_)) dirtyMat_ = true;
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirtyMat_);
        if (ImGui::Button("Save")) { SaveMaterial(db, editMat_); dirtyMat_ = false; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) { editMat_ = materials_[selMat_]; dirtyMat_ = false; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) DeleteMaterial(db, editMat_.id);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Select a material, or click \"New Material\".");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Anim Clips sub-tab
// ---------------------------------------------------------------------------

static bool DrawClipFields(MediaAnimClip& c) {
    bool changed = false;
    if (InputString("Name", c.name)) changed = true;
    if (PathField("Source", c.source_path, "Animation File",
                  "fbx,glb,gltf,dae,b3d", "anims")) changed = true;
    if (InputString("Clip Override", c.clip_override)) changed = true;
    ImGui::TextDisabled("Source empty = clip is embedded in the actor's Body model.");
    ImGui::TextDisabled("Clip Override empty = use first clip inside the source file.");
    return changed;
}

void MediaTab::DrawAnimClips(sqlite3* db) {
    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Clip")) {
        pendingClip_ = {};
        newClip_  = true;
        selClip_  = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    ImGui::BeginChild("##clip_list", {260, 0}, true);
    for (int i = 0; i < (int)clips_.size(); ++i) {
        char label[256];
        std::snprintf(label, sizeof(label), "%s##cl%d", clips_[i].name.c_str(), i);
        if (ImGui::Selectable(label, selClip_ == i)) {
            selClip_   = i;
            editClip_  = clips_[i];
            dirtyClip_ = false;
            newClip_   = false;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##clip_edit", {0, 0}, true);
    if (newClip_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Animation Clip");
        ImGui::Separator();
        DrawClipFields(pendingClip_);
        ImGui::Spacing();
        if (ImGui::Button("Create")) { SaveAnimClip(db, pendingClip_); newClip_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) newClip_ = false;
    } else if (selClip_ >= 0 && selClip_ < (int)clips_.size()) {
        ImGui::Text("Editing: [id=%d] %s", editClip_.id, editClip_.name.c_str());
        ImGui::Separator();
        if (DrawClipFields(editClip_)) dirtyClip_ = true;
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirtyClip_);
        if (ImGui::Button("Save")) { SaveAnimClip(db, editClip_); dirtyClip_ = false; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) { editClip_ = clips_[selClip_]; dirtyClip_ = false; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) DeleteAnimClip(db, editClip_.id);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Select a clip, or click \"New Clip\".");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Actor Defs sub-tab
// ---------------------------------------------------------------------------

static const char* lookupName(const std::vector<std::pair<int, std::string>>& v, int id,
                              const char* fallback) {
    for (auto& p : v) if (p.first == id) return p.second.c_str();
    return fallback;
}

void MediaTab::DrawActorDefs(sqlite3* db) {
    auto modelList = idNameList(models_);
    auto matList   = idNameList(materials_);
    auto clipList  = idNameList(clips_);

    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Actor Def")) {
        pendingActorDef_ = {};
        newActorDef_ = true;
        selActorDef_ = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    ImGui::BeginChild("##ad_list", {260, 0}, true);
    for (int i = 0; i < (int)actor_defs_.size(); ++i) {
        char label[256];
        std::snprintf(label, sizeof(label), "%s##adl%d",
                      actor_defs_[i].name.c_str(), i);
        if (ImGui::Selectable(label, selActorDef_ == i)) {
            selActorDef_   = i;
            editActorDef_  = actor_defs_[i];
            dirtyActorDef_ = false;
            newActorDef_   = false;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##ad_edit", {0, 0}, true);

    if (newActorDef_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Actor Def");
        ImGui::Separator();
        InputString("Name", pendingActorDef_.name);
        ImGui::TextDisabled("Create the actor def first, then open it to add mesh slots and animations.");
        ImGui::Spacing();
        if (ImGui::Button("Create")) { SaveActorDef(db, pendingActorDef_); newActorDef_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) newActorDef_ = false;
    }
    else if (selActorDef_ >= 0 && selActorDef_ < (int)actor_defs_.size()) {
        ActorDef& d = editActorDef_;
        ImGui::Text("Editing: [id=%d] %s", d.id, d.name.c_str());
        ImGui::Separator();

        if (InputString("Name", d.name)) dirtyActorDef_ = true;
        ImGui::Spacing();
        ImGui::BeginDisabled(!dirtyActorDef_);
        if (ImGui::Button("Save Name")) { SaveActorDef(db, d); dirtyActorDef_ = false; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete Def")) DeleteActorDef(db, d.id);
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();

        // --- Mesh Slots section ---
        ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Mesh Slots");
        ImGui::BeginChild("##meshes", {0, 220}, true);

        if (ImGui::BeginTable("##slot_tbl", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Model");
            ImGui::TableSetupColumn("Material");
            ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("##del",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < d.mesh_slots.size(); ++i) {
                ActorMeshSlot& s = d.mesh_slots[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ActorSlotName(s.slot));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(
                    lookupName(modelList, s.model_id, "(none)"));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(
                    s.material_id == 0 ? "(embedded)" :
                    lookupName(matList, s.material_id, "(?)"));

                ImGui::TableNextColumn();
                ImGui::PushID((int)i);
                if (ImGui::SmallButton("Edit")) {
                    // Inline-edit: put into a popup.
                    ImGui::OpenPopup("edit_slot");
                }
                if (ImGui::BeginPopup("edit_slot")) {
                    ImGui::Combo("Slot", &s.slot, kSlotNames, kSlotCount);
                    ComboId("Model",    s.model_id,    modelList);
                    ComboId("Material", s.material_id, matList, "(embedded)");
                    if (ImGui::Button("Save##es")) {
                        SaveMeshSlot(db, s);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Close##es")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                ImGui::PushID((int)(i + 10000));
                if (ImGui::SmallButton("Del")) DeleteMeshSlot(db, s.id);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Add slot:");
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("##nslot", &newSlotSlot_, kSlotNames, kSlotCount);
        ImGui::SameLine();

        int newModelId = newSlotModelIdx_ >= 0 && newSlotModelIdx_ < (int)modelList.size()
                       ? modelList[newSlotModelIdx_].first : 0;
        int newMatId   = newSlotMatIdx_   >= 0 && newSlotMatIdx_   < (int)matList.size()
                       ? matList[newSlotMatIdx_].first : 0;

        ImGui::SetNextItemWidth(160);
        if (ComboId("##nmdl", newModelId, modelList)) {
            for (int k = 0; k < (int)modelList.size(); ++k)
                if (modelList[k].first == newModelId) { newSlotModelIdx_ = k; break; }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ComboId("##nmat", newMatId, matList, "(embedded)")) {
            newSlotMatIdx_ = -1;
            for (int k = 0; k < (int)matList.size(); ++k)
                if (matList[k].first == newMatId) { newSlotMatIdx_ = k; break; }
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Slot")) {
            if (newModelId == 0) {
                std::snprintf(statusMsg_, sizeof(statusMsg_),
                              "Pick a model before adding a slot.");
            } else {
                ActorMeshSlot s;
                s.actor_def_id = d.id;
                s.slot         = newSlotSlot_;
                s.model_id     = newModelId;
                s.material_id  = newMatId;
                SaveMeshSlot(db, s);
                // reset picker so user sees the action completed
                newSlotModelIdx_ = -1;
                newSlotMatIdx_   = -1;
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();

        // --- Animation Map section ---
        ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Animation Mapping");
        ImGui::BeginChild("##anims", {0, 220}, true);

        if (ImGui::BeginTable("##anim_tbl", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Action");
            ImGui::TableSetupColumn("Clip");
            ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("##del",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < d.anim_map.size(); ++i) {
                ActorAnimMap& a = d.anim_map[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.action.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(
                    lookupName(clipList, a.clip_id, "(none)"));

                ImGui::TableNextColumn();
                ImGui::PushID((int)(i + 20000));
                if (ImGui::SmallButton("Edit")) ImGui::OpenPopup("edit_anim");
                if (ImGui::BeginPopup("edit_anim")) {
                    InputString("Action", a.action, 64);
                    ComboId("Clip", a.clip_id, clipList);
                    if (ImGui::Button("Save##ea")) {
                        SaveAnimMap(db, a);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Close##ea")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                ImGui::PushID((int)(i + 30000));
                if (ImGui::SmallButton("Del")) DeleteAnimMap(db, a.id);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Add mapping:");
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("##nact", "Idle, Walk, Attack, Death...",
                                 newAnimAction_, sizeof(newAnimAction_));
        ImGui::SameLine();

        int newClipId = newAnimClipIdx_ >= 0 && newAnimClipIdx_ < (int)clipList.size()
                      ? clipList[newAnimClipIdx_].first : 0;
        ImGui::SetNextItemWidth(180);
        if (ComboId("##nclip", newClipId, clipList)) {
            newAnimClipIdx_ = -1;
            for (int k = 0; k < (int)clipList.size(); ++k)
                if (clipList[k].first == newClipId) { newAnimClipIdx_ = k; break; }
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Mapping")) {
            if (!newAnimAction_[0] || newClipId == 0) {
                std::snprintf(statusMsg_, sizeof(statusMsg_),
                              "Need both Action name and Clip to add a mapping.");
            } else {
                ActorAnimMap a;
                a.actor_def_id = d.id;
                a.action       = newAnimAction_;
                a.clip_id      = newClipId;
                SaveAnimMap(db, a);
                newAnimAction_[0] = 0;
                newAnimClipIdx_   = -1;
            }
        }
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("Select an actor def, or click \"New Actor Def\".");
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Main Draw
// ---------------------------------------------------------------------------

void MediaTab::Draw(sqlite3* db) {
    if (needFetch_) {
        // Before refetch, remember which actor def is currently being edited
        // by id. After refetch, re-select it and resync editActorDef_ — otherwise
        // edits to child tables (mesh_slots, anim_map) wouldn't show up in the
        // UI until the user manually re-clicked the def in the list.
        int keepActorDefId = -1;
        if (selActorDef_ >= 0 && selActorDef_ < (int)actor_defs_.size())
            keepActorDefId = actor_defs_[selActorDef_].id;

        FetchAll(db);
        needFetch_ = false;

        if (keepActorDefId > 0) {
            selActorDef_ = -1;
            for (int i = 0; i < (int)actor_defs_.size(); ++i) {
                if (actor_defs_[i].id == keepActorDefId) {
                    selActorDef_  = i;
                    editActorDef_ = actor_defs_[i];
                    break;
                }
            }
        }
    }

    if (ImGui::BeginTabBar("##media_subtabs")) {
        if (ImGui::BeginTabItem("Models"))     { DrawModels    (db); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Materials"))  { DrawMaterials (db); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Anim Clips")) { DrawAnimClips (db); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Actor Defs")) { DrawActorDefs (db); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

} // namespace gue
