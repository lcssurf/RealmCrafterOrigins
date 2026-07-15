#include "items.h"
#include "../attribute_list.h"
#include "../ui_widgets.h"
#include "../file_import.h"
#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <cstdio>

namespace gue {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char* kItemTypes[]   = { "Weapon", "Armor", "Consumable", "Misc" };
static const char* kSlotTypes[]   = {
    "Weapon (0)", "Shield (1)", "Hat (2)", "Chest (3)", "Hands (4)",
    "Belt (5)",   "Legs (6)",   "Feet (7)", "Ring (8)", "Amulet (9)",
};
static const char* kWeaponDimensions[] = { "Melee", "Ranged", "Magic" };
static const char* kWeaponHands[]      = { "One-Hand", "Two-Hand" };

// ---------------------------------------------------------------------------
// DB helpers
// ---------------------------------------------------------------------------

bool ItemsTab::LoadItemAttributes(sqlite3* db, ItemTemplate& item) {
    item.attributes.clear();

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT attribute_key, value"
        " FROM item_attributes WHERE item_id=? ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "LoadItemAttributes error: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(stmt, 1, item.id);

    while (true) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                          "LoadItemAttributes read error: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return false;
        }

        ItemAttribute attr;
        const unsigned char* key = sqlite3_column_text(stmt, 0);
        attr.key = key ? reinterpret_cast<const char*>(key) : "";
        attr.value = sqlite3_column_double(stmt, 1);
        item.attributes.push_back(std::move(attr));
    }

    sqlite3_finalize(stmt);
    return true;
}

bool ItemsTab::LoadItemOverrides(sqlite3* db, ItemTemplate& item) {
    item.overrides.clear();

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, item_template_id, actor_def_id,"
        "       offset_pos_x, offset_pos_y, offset_pos_z,"
        "       offset_rot_x, offset_rot_y, offset_rot_z, offset_scale "
        "FROM item_socket_overrides WHERE item_template_id=? ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "LoadItemOverrides error: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(stmt, 1, item.id);

    while (true) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                          "LoadItemOverrides read error: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return false;
        }

        ItemSocketOverride o;
        o.id = sqlite3_column_int(stmt, 0);
        o.item_template_id = sqlite3_column_int(stmt, 1);
        o.actor_def_id = sqlite3_column_int(stmt, 2);
        o.offset_pos_x = static_cast<float>(sqlite3_column_double(stmt, 3));
        o.offset_pos_y = static_cast<float>(sqlite3_column_double(stmt, 4));
        o.offset_pos_z = static_cast<float>(sqlite3_column_double(stmt, 5));
        o.offset_rot_x = static_cast<float>(sqlite3_column_double(stmt, 6));
        o.offset_rot_y = static_cast<float>(sqlite3_column_double(stmt, 7));
        o.offset_rot_z = static_cast<float>(sqlite3_column_double(stmt, 8));
        o.offset_scale = static_cast<float>(sqlite3_column_double(stmt, 9));
        item.overrides.push_back(std::move(o));
    }

    sqlite3_finalize(stmt);
    return true;
}

bool ItemsTab::SaveItemAttributes(sqlite3* db, const ItemTemplate& item) {
    if (item.id <= 0) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemAttributes error: invalid item id.");
        return false;
    }

    if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemAttributes: begin transaction failed: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM item_attributes WHERE item_id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemAttributes: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int(stmt, 1, item.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemAttributes: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(db,
        "INSERT INTO item_attributes (item_id, attribute_key, value)"
        " VALUES (?, ?, ?)", -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemAttributes: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    for (const auto& attr : item.attributes) {
        const std::string& key = attr.key;
        if (key.empty()) continue;
        if (!IsKnownAttributeKey(key)) continue;

        sqlite3_bind_int(stmt, 1, item.id);
        sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, attr.value);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                          "SaveItemAttributes insert error: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemAttributes: commit failed: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

bool ItemsTab::SaveItemOverrides(sqlite3* db, const ItemTemplate& item) {
    if (item.id <= 0) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemOverrides error: invalid item id.");
        return false;
    }

    if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemOverrides: begin transaction failed: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM item_socket_overrides WHERE item_template_id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemOverrides: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int(stmt, 1, item.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemOverrides: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_finalize(stmt);

    if (item.overrides.empty()) {
        if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                          "SaveItemOverrides: commit failed: %s", sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        return true;
    }

    if (sqlite3_prepare_v2(db,
        "INSERT INTO item_socket_overrides "
        "(item_template_id, actor_def_id, offset_pos_x, offset_pos_y, offset_pos_z, "
        " offset_rot_x, offset_rot_y, offset_rot_z, offset_scale)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemOverrides: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    for (const auto& o : item.overrides) {
        if (o.actor_def_id <= 0) continue;
        sqlite3_bind_int(stmt, 1, item.id);
        sqlite3_bind_int(stmt, 2, o.actor_def_id);
        sqlite3_bind_double(stmt, 3, o.offset_pos_x);
        sqlite3_bind_double(stmt, 4, o.offset_pos_y);
        sqlite3_bind_double(stmt, 5, o.offset_pos_z);
        sqlite3_bind_double(stmt, 6, o.offset_rot_x);
        sqlite3_bind_double(stmt, 7, o.offset_rot_y);
        sqlite3_bind_double(stmt, 8, o.offset_rot_z);
        sqlite3_bind_double(stmt, 9, o.offset_scale);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                          "SaveItemOverrides insert error: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "SaveItemOverrides: commit failed: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

bool ItemsTab::LoadSocketVocabulary(sqlite3* db) {
    socketVocab_.clear();

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT name FROM socket_vocabulary ORDER BY name";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* n = sqlite3_column_text(stmt, 0);
        socketVocab_.push_back(n ? reinterpret_cast<const char*>(n) : "");
    }
    sqlite3_finalize(stmt);
    return true;
}

bool ItemsTab::LoadActorDefs(sqlite3* db) {
    actorDefOptions_.clear();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name FROM media_actor_defs ORDER BY name";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WeaponKitOption opt{};
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        actorDefOptions_.push_back({id, name ? reinterpret_cast<const char*>(name) : ""});
    }
    sqlite3_finalize(stmt);
    return true;
}

bool ItemsTab::LoadModelOptions(sqlite3* db) {
    modelOptions_.clear();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, file_path FROM media_models ORDER BY name";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ModelOption m;
        m.id = sqlite3_column_int(stmt, 0);
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        const unsigned char* path = sqlite3_column_text(stmt, 2);
        m.name      = name ? reinterpret_cast<const char*>(name) : "";
        m.file_path = path ? reinterpret_cast<const char*>(path) : "";
        modelOptions_.push_back(std::move(m));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool ItemsTab::ResolveActorDefSocket(sqlite3* db, int actor_def_id,
                                     const std::string& socket_name,
                                     SocketResolution& out) {
    if (actor_def_id <= 0) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT mm.file_path"
        " FROM media_actor_meshes am"
        " JOIN media_models mm ON mm.id = am.model_id"
        " WHERE am.actor_def_id = ?"
        " ORDER BY (am.slot != 0), am.id LIMIT 1",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(stmt, 1, actor_def_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* p = sqlite3_column_text(stmt, 0);
        out.body_model_path = p ? reinterpret_cast<const char*>(p) : "";
    }
    sqlite3_finalize(stmt);
    if (out.body_model_path.empty()) return false;

    if (sqlite3_prepare_v2(db,
        "SELECT bone_name, offset_pos_x, offset_pos_y, offset_pos_z,"
        "       offset_rot_x, offset_rot_y, offset_rot_z, offset_scale"
        " FROM actor_def_sockets WHERE actor_def_id=? AND socket_name=? LIMIT 1",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(stmt, 1, actor_def_id);
    sqlite3_bind_text(stmt, 2, socket_name.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* bone = sqlite3_column_text(stmt, 0);
        out.bone_name    = bone ? reinterpret_cast<const char*>(bone) : "";
        out.offset_pos_x = (float)sqlite3_column_double(stmt, 1);
        out.offset_pos_y = (float)sqlite3_column_double(stmt, 2);
        out.offset_pos_z = (float)sqlite3_column_double(stmt, 3);
        out.offset_rot_x = (float)sqlite3_column_double(stmt, 4);
        out.offset_rot_y = (float)sqlite3_column_double(stmt, 5);
        out.offset_rot_z = (float)sqlite3_column_double(stmt, 6);
        out.offset_scale = (float)sqlite3_column_double(stmt, 7);
        found = !out.bone_name.empty();
    }
    sqlite3_finalize(stmt);
    return found;
}

void ItemsTab::Fetch(sqlite3* db) {
    items_.clear();
    selected_ = -1;
    iconOptions_ = gue::ListTextureAssets();

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, name, item_type, slot_type, weapon_damage, armor_level, "
        "       weapon_dimension, weapon_hands, weapon_range, max_stack, item_value, stackable, weapon_kit "
        "       , model_path, model_scale, socket_name, icon_path "
        "FROM item_templates ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Fetch error: %s", sqlite3_errmsg(db));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ItemTemplate t;
        t.id            = sqlite3_column_int(stmt, 0);
        t.name          = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        t.item_type     = sqlite3_column_int(stmt, 2);
        t.slot_type     = sqlite3_column_int(stmt, 3);
        t.weapon_damage = sqlite3_column_int(stmt, 4);
        t.armor_level   = sqlite3_column_int(stmt, 5);
        t.weapon_dimension = sqlite3_column_int(stmt, 6);
        t.weapon_hands     = sqlite3_column_int(stmt, 7);
        t.weapon_range  = (float)sqlite3_column_double(stmt, 8);
        t.max_stack     = sqlite3_column_int(stmt, 9);
        t.item_value    = sqlite3_column_int(stmt, 10);
        t.stackable     = sqlite3_column_int(stmt, 11) != 0;
        const char* wk  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        t.weapon_kit    = wk ? wk : "";
        const char* mp  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        t.model_path = mp ? mp : "";
        t.model_scale = static_cast<float>(sqlite3_column_double(stmt, 14));
        const char* sock = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
        t.socket_name = sock ? sock : "";
        const char* icon = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
        t.icon_path = icon ? icon : "";
        items_.push_back(t);
    }
    sqlite3_finalize(stmt);
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Loaded %d items.", (int)items_.size());
}

void ItemsTab::FetchWeaponKitOptions(sqlite3* db) {
    weapon_kit_options_.clear();

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT kit_key, display_name FROM weapon_kits "
        "WHERE enabled=1 ORDER BY kit_key";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WeaponKitOption opt;
        const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* d = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        opt.kit_key = k ? k : "";
        opt.display_name = d ? d : "";
        weapon_kit_options_.push_back(opt);
    }
    sqlite3_finalize(stmt);
}

bool ItemsTab::Save(sqlite3* db, ItemTemplate& t) {
    sqlite3_stmt* stmt = nullptr;
    int rc;

    if (t.id == 0) {
        // INSERT
        const char* sql =
            "INSERT INTO item_templates "
            "(name, item_type, slot_type, weapon_damage, armor_level, "
            " weapon_dimension, weapon_hands, weapon_range, max_stack, item_value, stackable, weapon_kit, model_path, model_scale, socket_name, icon_path) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt, 1, t.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,  2, t.item_type);
        sqlite3_bind_int(stmt,  3, t.slot_type);
        sqlite3_bind_int(stmt,  4, t.weapon_damage);
        sqlite3_bind_int(stmt,  5, t.armor_level);
        sqlite3_bind_int(stmt,  6, t.weapon_dimension);
        sqlite3_bind_int(stmt,  7, t.weapon_hands);
        sqlite3_bind_double(stmt, 8, (double)t.weapon_range);
        sqlite3_bind_int(stmt,  9, t.max_stack);
        sqlite3_bind_int(stmt, 10, t.item_value);
        sqlite3_bind_int(stmt, 11, t.stackable ? 1 : 0);
        sqlite3_bind_text(stmt, 12, t.weapon_kit.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 13, t.model_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 14, static_cast<double>(t.model_scale));
        sqlite3_bind_text(stmt, 15, t.socket_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 16, t.icon_path.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
        t.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        // UPDATE
        const char* sql =
            "UPDATE item_templates SET "
            "name=?, item_type=?, slot_type=?, weapon_damage=?, armor_level=?, "
            "weapon_dimension=?, weapon_hands=?, weapon_range=?, max_stack=?, item_value=?, stackable=?, weapon_kit=?, model_path=?, model_scale=?, socket_name=?, icon_path=? "
            "WHERE id=?";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt, 1, t.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,  2, t.item_type);
        sqlite3_bind_int(stmt,  3, t.slot_type);
        sqlite3_bind_int(stmt,  4, t.weapon_damage);
        sqlite3_bind_int(stmt,  5, t.armor_level);
        sqlite3_bind_int(stmt,  6, t.weapon_dimension);
        sqlite3_bind_int(stmt,  7, t.weapon_hands);
        sqlite3_bind_double(stmt, 8, (double)t.weapon_range);
        sqlite3_bind_int(stmt,  9, t.max_stack);
        sqlite3_bind_int(stmt, 10, t.item_value);
        sqlite3_bind_int(stmt, 11, t.stackable ? 1 : 0);
        sqlite3_bind_text(stmt, 12, t.weapon_kit.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 13, t.model_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 14, static_cast<double>(t.model_scale));
        sqlite3_bind_text(stmt, 15, t.socket_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 16, t.icon_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 17, t.id);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
    }

    sqlite3_finalize(stmt);
    if (!SaveItemAttributes(db, t)) {
        return false;
    }
    if (!SaveItemOverrides(db, t)) {
        return false;
    }
    needFetch_ = true;
    dirty_     = false;
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Saved '%s' (id=%d).", t.name.c_str(), t.id);
    return true;

err:
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Save error: %s", sqlite3_errmsg(db));
    if (stmt) sqlite3_finalize(stmt);
    return false;
}

bool ItemsTab::Delete(sqlite3* db, int id) {
    // Guard: reject if item is in use
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM character_items WHERE item_id=?",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (count > 0) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Cannot delete: item %d is in %d inventory slot(s).", id, count);
        return false;
    }

    sqlite3_prepare_v2(db, "DELETE FROM item_socket_overrides WHERE item_template_id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_prepare_v2(db, "DELETE FROM item_templates WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }

    needFetch_ = true;
    selected_  = -1;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted item %d.", id);
    return true;
}

// ---------------------------------------------------------------------------
// DrawFields — shared editor form
// ---------------------------------------------------------------------------

bool ItemsTab::DrawFields(ItemTemplate& t) {
    bool changed = false;

    char buf[128];
    std::strncpy(buf, t.name.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    if (ImGui::InputText("Name", buf, sizeof(buf))) { t.name = buf; changed = true; }

    if (ImGui::Combo("Type", &t.item_type, kItemTypes, 4)) changed = true;

    if (t.item_type == 0 || t.item_type == 1) {
        int st = (t.slot_type < 10) ? t.slot_type : 0;
        // New items (or items just switched to weapon/armor) start with
        // slot_type=255 (bag-only). The combo shows the fallback "st" value,
        // but slot_type stayed 255 unless the user clicks the combo, so it
        // saved as bag-only and could never be equipped. Sync it now.
        if (t.slot_type >= 10) {
            t.slot_type = st;
            changed = true;
        }
        if (ImGui::Combo("Equip Slot", &st, kSlotTypes, 10)) {
            t.slot_type = st; changed = true;
        }
    } else {
        t.slot_type = 255;
    }

    if (t.item_type == 0) {
        if (ImGui::InputInt("Damage",      &t.weapon_damage)) changed = true;
        if (ImGui::Combo("Dimension",      &t.weapon_dimension, kWeaponDimensions, 3)) changed = true;
        int hands = (t.weapon_hands == 2) ? 1 : 0;
        if (ImGui::Combo("Hands", &hands, kWeaponHands, 2)) {
            t.weapon_hands = hands + 1;
            changed = true;
        }
        if (ImGui::InputFloat("Attack Range", &t.weapon_range, 0.5f, 1.0f, "%.1f")) changed = true;
        ImGui::TextDisabled("0 = use dimension default (melee 2 / ranged 15 / magic 12)");
    }
    if (t.item_type == 1) {
        if (ImGui::InputInt("Armor Level", &t.armor_level)) changed = true;
    }

    // Visible for all item types: any item may grant a kit in future design.
    {
        static char current_label_buf[256];
        std::strcpy(current_label_buf, "(none)");
        if (!t.weapon_kit.empty()) {
            bool found = false;
            for (const auto& opt : weapon_kit_options_) {
                if (opt.kit_key == t.weapon_kit) {
                    std::snprintf(current_label_buf, sizeof(current_label_buf), "%s [%s]",
                        opt.display_name.empty() ? opt.kit_key.c_str() : opt.display_name.c_str(),
                        opt.kit_key.c_str());
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::snprintf(current_label_buf, sizeof(current_label_buf), "[%s] (missing)",
                    t.weapon_kit.c_str());
            }
        }
        const char* current_label = current_label_buf;

        if (ImGui::BeginCombo("Weapon Kit", current_label)) {
            const bool none_sel = t.weapon_kit.empty();
            if (ImGui::Selectable("(none)", none_sel)) {
                t.weapon_kit = "";
                changed = true;
            }

            for (const auto& opt : weapon_kit_options_) {
                const bool sel = (opt.kit_key == t.weapon_kit);
                char label[256];
                std::snprintf(label, sizeof(label), "%s [%s]",
                    opt.display_name.empty() ? opt.kit_key.c_str() : opt.display_name.c_str(),
                    opt.kit_key.c_str());
                if (ImGui::Selectable(label, sel)) {
                    t.weapon_kit = opt.kit_key;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Skills granted to the player when this item is equipped.\nOptional. Leave (none) for items that don't provide a kit.");
        }
    }

    if (ImGui::InputInt("Value (gold)",    &t.item_value))  changed = true;
    if (ImGui::InputInt("Max Stack",       &t.max_stack))   changed = true;
    if (ImGui::Checkbox("Stackable",       &t.stackable))   changed = true;

    ImGui::SeparatorText("Render Attachment");
    if (modelOptions_.empty()) {
        char modelPathBuf[260];
        std::strncpy(modelPathBuf, t.model_path.c_str(), sizeof(modelPathBuf)-1);
        modelPathBuf[sizeof(modelPathBuf)-1] = 0;
        if (ImGui::InputText("Model Path", modelPathBuf, sizeof(modelPathBuf))) {
            t.model_path = modelPathBuf;
            changed = true;
        }
        ImGui::TextDisabled("No models registered in Assets > Models yet.");
    } else {
        // Same searchable-by-name picker used for models elsewhere (e.g.
        // Actor Def mesh slots) instead of a raw path InputText.
        int modelId = 0;
        std::vector<std::pair<int, std::string>> modelPairs;
        modelPairs.reserve(modelOptions_.size());
        for (const auto& m : modelOptions_) {
            modelPairs.push_back({m.id, m.name});
            if (m.file_path == t.model_path) modelId = m.id;
        }
        if (gue::ui::SearchableComboId("Model", modelId, modelPairs, "(none)")) {
            for (const auto& m : modelOptions_) {
                if (m.id == modelId) { t.model_path = m.file_path; break; }
            }
            if (modelId == 0) t.model_path.clear();
            changed = true;
        }
        if (!t.model_path.empty())
            ImGui::TextDisabled("%s", t.model_path.c_str());
    }
    if (ImGui::InputFloat("Model Scale", &t.model_scale, 0.05f, 0.1f, "%.3f")) {
        changed = true;
    }

    ImGui::SeparatorText("Inventory Icon");
    if (gue::ui::SearchableComboString("Icon", t.icon_path, iconOptions_, "(none)")) {
        changed = true;
    }
    if (!t.icon_path.empty())
        ImGui::TextDisabled("%s", t.icon_path.c_str());
    if (ImGui::Button("Import Icon...")) {
        std::string picked = gue::PickAndImportAsset(
            "Icon Image", "png,jpg,jpeg,bmp", "models/Textures/Item Icons");
        if (!picked.empty()) {
            t.icon_path = picked;
            iconOptions_ = gue::ListTextureAssets();
            changed = true;
        }
    }

    if (socketVocab_.empty()) {
        char sockBuf[128];
        std::strncpy(sockBuf, t.socket_name.c_str(), sizeof(sockBuf)-1);
        sockBuf[sizeof(sockBuf)-1] = 0;
        if (ImGui::InputText("Socket", sockBuf, sizeof(sockBuf))) {
            t.socket_name = sockBuf;
            changed = true;
        }
    } else if (gue::ui::SearchableComboString("Socket", t.socket_name, socketVocab_, "(none)")) {
        changed = true;
    }

    ImGui::SeparatorText("Per-actor Overrides");
    for (size_t i = 0; i < t.overrides.size();) {
        auto& o = t.overrides[i];
        ImGui::PushID((int)i);
        bool removed = false;

        int actorDefID = o.actor_def_id;
        if (gue::ui::SearchableComboId("Actor Def", actorDefID, actorDefOptions_, "(none)")) {
            o.actor_def_id = actorDefID;
            changed = true;
        }

        float pos[3] = {o.offset_pos_x, o.offset_pos_y, o.offset_pos_z};
        if (ImGui::SliderFloat3("Offset Pos", pos, -1.5f, 1.5f, "%.3f")) {
            o.offset_pos_x = pos[0];
            o.offset_pos_y = pos[1];
            o.offset_pos_z = pos[2];
            changed = true;
        }
        float rot[3] = {o.offset_rot_x, o.offset_rot_y, o.offset_rot_z};
        if (ImGui::SliderFloat3("Offset Rot", rot, -180.f, 180.f, "%.0f")) {
            o.offset_rot_x = rot[0];
            o.offset_rot_y = rot[1];
            o.offset_rot_z = rot[2];
            changed = true;
        }
        if (ImGui::SliderFloat("Offset Scale", &o.offset_scale, 0.05f, 3.f, "%.3f")) {
            changed = true;
        }

        bool previewing = (previewOverrideIdx_ == (int)i);
        if (previewing) ImGui::PushStyleColor(ImGuiCol_Button, {0.2f, 0.55f, 0.25f, 1.f});
        if (ImGui::Button(previewing ? "Previewing" : "Preview")) {
            previewOverrideIdx_ = previewing ? -1 : (int)i;
        }
        if (previewing) ImGui::PopStyleColor();
        ImGui::SameLine();

        if (ImGui::Button("Remove")) {
            if (previewOverrideIdx_ == (int)i) previewOverrideIdx_ = -1;
            t.overrides.erase(t.overrides.begin() + (int)i);
            removed = true;
            changed = true;
        }
        if (removed) {
            ImGui::PopID();
            continue;
        }

        ImGui::Separator();
        ImGui::PopID();
        ++i;
    }

    if (ImGui::Button("+ Add Override")) {
        ItemSocketOverride o;
        o.item_template_id = t.id;
        t.overrides.push_back(o);
        changed = true;
    }

    static const auto displayNames = AttributeDisplayNames();
    ImGui::SeparatorText("Attributes");
    for (size_t i = 0; i < t.attributes.size();) {
        auto& attr = t.attributes[i];
        ImGui::PushID((int)i);

        bool removed = false;
        std::string display = AttributeDisplayFromKey(attr.key);
        if (gue::ui::SearchableComboString("Attribute", display, displayNames, "(select)")) {
            attr.key = AttributeKeyFromDisplay(display);
            changed = true;
        }

        float value = static_cast<float>(attr.value);
        if (ImGui::InputFloat("Value", &value, 0.f, 0.f, "%.2f")) {
            attr.value = static_cast<double>(value);
            changed = true;
        }

        if (ImGui::Button("Remove")) {
            t.attributes.erase(t.attributes.begin() + (int)i);
            changed = true;
            removed = true;
        }
        ImGui::PopID();
        if (removed) continue;
        ++i;
    }
    if (ImGui::Button("+ Add Attribute")) {
        t.attributes.push_back({"", 0.0});
        changed = true;
    }

    if (t.weapon_damage < 0)  t.weapon_damage = 0;
    if (t.armor_level   < 0)  t.armor_level   = 0;
    if (t.item_value    < 0)  t.item_value    = 0;
    if (t.max_stack     < 1)  t.max_stack     = 1;
    if (t.max_stack     > 99) t.max_stack     = 99;
    if (t.model_scale   <= 0.f) t.model_scale   = 1.f;

    return changed;
}

// ---------------------------------------------------------------------------
// Item-on-socket preview
// ---------------------------------------------------------------------------

void ItemsTab::DrawItemPreview(sqlite3* db, ItemTemplate& t) {
    if (!preview_) {
        if (!engine_ || !pipeline_) {
            preview_init_ok_ = false;
        } else {
            preview_ = std::make_unique<PreviewViewport>();
            preview_->Init(engine_, pipeline_);
            preview_init_ok_ = true;
        }
    }
    if (!preview_init_ok_) {
        ImGui::TextDisabled("Preview unavailable (renderer not ready).");
        return;
    }

    if (previewOverrideIdx_ < 0 || previewOverrideIdx_ >= (int)t.overrides.size()) {
        preview_->ClearAttachment();
        preview_->Clear();
        ImGui::TextDisabled(
            "Click \"Preview\" on a Per-actor Override below to see the item "
            "attached to that actor's socket.");
        return;
    }

    const ItemSocketOverride& ov = t.overrides[previewOverrideIdx_];

    if (t.socket_name.empty()) {
        preview_->ClearAttachment();
        ImGui::TextDisabled("Set the item's Socket to preview it.");
        return;
    }
    if (t.model_path.empty()) {
        preview_->ClearAttachment();
        ImGui::TextDisabled("Set the item's Model Path to preview it.");
        return;
    }

    SocketResolution sock;
    if (!ResolveActorDefSocket(db, ov.actor_def_id, t.socket_name, sock)) {
        preview_->ClearAttachment();
        ImGui::TextDisabled(
            "Actor def has no socket binding for '%s'. Add one in "
            "Assets > Actor Defs > Sockets.", t.socket_name.c_str());
        return;
    }

    if (preview_->CurrentPath() != sock.body_model_path)
        preview_->LoadModel(sock.body_model_path);

    // socketMat: the actor def's socket-binding offset (bone -> socket).
    glm::mat4 socketMat(1.0f);
    socketMat = glm::translate(socketMat,
        glm::vec3(sock.offset_pos_x, sock.offset_pos_y, sock.offset_pos_z));
    socketMat = glm::rotate(socketMat, glm::radians(sock.offset_rot_x), glm::vec3(1, 0, 0));
    socketMat = glm::rotate(socketMat, glm::radians(sock.offset_rot_y), glm::vec3(0, 1, 0));
    socketMat = glm::rotate(socketMat, glm::radians(sock.offset_rot_z), glm::vec3(0, 0, 1));
    socketMat = glm::scale(socketMat, glm::vec3(sock.offset_scale > 0.f ? sock.offset_scale : 1.f));

    // itemMat: this item's per-actor fine-tuning offset on top of the socket.
    glm::mat4 itemMat(1.0f);
    itemMat = glm::translate(itemMat,
        glm::vec3(ov.offset_pos_x, ov.offset_pos_y, ov.offset_pos_z));
    itemMat = glm::rotate(itemMat, glm::radians(ov.offset_rot_x), glm::vec3(1, 0, 0));
    itemMat = glm::rotate(itemMat, glm::radians(ov.offset_rot_y), glm::vec3(0, 1, 0));
    itemMat = glm::rotate(itemMat, glm::radians(ov.offset_rot_z), glm::vec3(0, 0, 1));
    const float itemScale = (ov.offset_scale > 0.f ? ov.offset_scale : 1.f) *
                             (t.model_scale  > 0.f ? t.model_scale  : 1.f);
    itemMat = glm::scale(itemMat, glm::vec3(itemScale));

    PreviewViewport::AttachmentSpec spec;
    spec.model_path     = t.model_path;
    spec.bone_name      = sock.bone_name;
    spec.local_transform = socketMat * itemMat;
    preview_->SetAttachment(spec);

    ImGui::Text("Socket '%s' -> bone '%s'", t.socket_name.c_str(), sock.bone_name.c_str());
    preview_->DrawImGui();
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void ItemsTab::Draw(sqlite3* db) {
    if (needFetch_) { Fetch(db); needFetch_ = false; }
    FetchWeaponKitOptions(db);
    LoadSocketVocabulary(db);
    LoadActorDefs(db);
    LoadModelOptions(db);

    if (ImGui::Button("Refresh")) { needFetch_ = true; }
    ImGui::SameLine();
    if (ImGui::Button("New Item")) { newItem_ = {}; showNew_ = true; selected_ = -1; previewOverrideIdx_ = -1; }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    float listW = 240.f;
    ImGui::BeginChild("##item_list", {listW, 0}, true);
    for (int i = 0; i < (int)items_.size(); ++i) {
        auto& it = items_[i];
        const char* typeIcon = (it.item_type == 0) ? "[W]"
                             : (it.item_type == 1) ? "[A]"
                             : (it.item_type == 2) ? "[C]" : "[M]";
        char label[160];
        std::snprintf(label, sizeof(label), "%s %s##li%d", typeIcon, it.name.c_str(), i);
        if (ImGui::Selectable(label, selected_ == i)) {
            selected_ = i;
            editing_  = it;
            LoadItemAttributes(db, editing_);
            LoadItemOverrides(db, editing_);
            dirty_    = false;
            showNew_  = false;
            previewOverrideIdx_ = -1;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##item_edit", {520.f, 0}, true);

    if (showNew_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Item");
        ImGui::Separator();
        DrawFields(newItem_);
        ImGui::Spacing();
        if (ImGui::Button("Create")) {
            if (Save(db, newItem_)) showNew_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) showNew_ = false;

    } else if (selected_ >= 0 && selected_ < (int)items_.size()) {
        ImGui::Text("Editing: [id=%d]  %s", editing_.id, editing_.name.c_str());
        ImGui::Separator();
        if (DrawFields(editing_)) dirty_ = true;
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Save"))   Save(db, editing_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            editing_ = items_[selected_];
            LoadItemAttributes(db, editing_);
            LoadItemOverrides(db, editing_);
            dirty_ = false;
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) Delete(db, editing_.id);
        ImGui::PopStyleColor();

    } else {
        ImGui::TextDisabled("Select an item, or click \"New Item\".");
    }

    ImGui::EndChild();

    // --- Item-on-socket preview: actor def body + item attached at the
    // chosen override's socket, so the dev can confirm the bone/offsets
    // without alt-tabbing into the client. ---
    ImGui::SameLine();
    ImGui::BeginChild("##item_preview", {0, 0}, true);
    ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Preview");
    ImGui::Separator();
    if (showNew_) {
        ImGui::TextDisabled("Save the item first, then add a Per-actor Override to preview it.");
    } else if (selected_ >= 0 && selected_ < (int)items_.size()) {
        DrawItemPreview(db, editing_);
    } else {
        ImGui::TextDisabled("Select an item to preview it here.");
    }
    ImGui::EndChild();
}

} // namespace gue
