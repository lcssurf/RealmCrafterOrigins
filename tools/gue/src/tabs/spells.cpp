#include "spells.h"
#include <imgui.h>
#include <cstring>
#include <cstdio>

namespace gue {

static const char* kSpellTypes[]  = { "Damage", "Heal", "Buff", "Debuff" };
static const char* kAoETypes[]    = { "Single Target", "AoE Around Target", "Ground-Targeted AoE" };

// ---------------------------------------------------------------------------
// DB helpers
// ---------------------------------------------------------------------------

void SpellsTab::Fetch(sqlite3* db) {
    spells_.clear();
    selected_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, name, spell_type, damage_min, damage_max, "
        "       ep_cost, cooldown_ms, range, icon, aoe_type, aoe_radius "
        "FROM spell_templates ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Fetch error: %s", sqlite3_errmsg(db));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SpellTemplate t;
        t.id          = sqlite3_column_int(stmt, 0);
        t.name        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        t.spell_type  = sqlite3_column_int(stmt, 2);
        t.damage_min  = sqlite3_column_int(stmt, 3);
        t.damage_max  = sqlite3_column_int(stmt, 4);
        t.ep_cost     = sqlite3_column_int(stmt, 5);
        t.cooldown_ms = sqlite3_column_int(stmt, 6);
        t.range       = static_cast<float>(sqlite3_column_double(stmt, 7));
        t.icon        = sqlite3_column_int(stmt, 8);
        t.aoe_type    = sqlite3_column_int(stmt, 9);
        t.aoe_radius  = static_cast<float>(sqlite3_column_double(stmt, 10));
        spells_.push_back(t);
    }
    sqlite3_finalize(stmt);
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Loaded %d spells.", (int)spells_.size());
}

bool SpellsTab::Save(sqlite3* db, SpellTemplate& t) {
    sqlite3_stmt* stmt = nullptr;
    int rc;

    if (t.id == 0) {
        const char* sql =
            "INSERT INTO spell_templates "
            "(name, spell_type, damage_min, damage_max, ep_cost, cooldown_ms, range, icon, aoe_type, aoe_radius) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt, 1, t.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,  2, t.spell_type);
        sqlite3_bind_int(stmt,  3, t.damage_min);
        sqlite3_bind_int(stmt,  4, t.damage_max);
        sqlite3_bind_int(stmt,  5, t.ep_cost);
        sqlite3_bind_int(stmt,  6, t.cooldown_ms);
        sqlite3_bind_double(stmt, 7, t.range);
        sqlite3_bind_int(stmt,  8, t.icon);
        sqlite3_bind_int(stmt,  9, t.aoe_type);
        sqlite3_bind_double(stmt, 10, t.aoe_radius);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
        t.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        const char* sql =
            "UPDATE spell_templates SET "
            "name=?, spell_type=?, damage_min=?, damage_max=?, "
            "ep_cost=?, cooldown_ms=?, range=?, icon=?, aoe_type=?, aoe_radius=? "
            "WHERE id=?";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) goto err;
        sqlite3_bind_text(stmt, 1, t.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,  2, t.spell_type);
        sqlite3_bind_int(stmt,  3, t.damage_min);
        sqlite3_bind_int(stmt,  4, t.damage_max);
        sqlite3_bind_int(stmt,  5, t.ep_cost);
        sqlite3_bind_int(stmt,  6, t.cooldown_ms);
        sqlite3_bind_double(stmt, 7, t.range);
        sqlite3_bind_int(stmt,  8, t.icon);
        sqlite3_bind_int(stmt,  9, t.aoe_type);
        sqlite3_bind_double(stmt, 10, t.aoe_radius);
        sqlite3_bind_int(stmt, 11, t.id);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto err;
    }

    sqlite3_finalize(stmt);
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

bool SpellsTab::Delete(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM character_known_spells WHERE spell_id=?",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (count > 0) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Cannot delete: spell %d is known by %d character(s).", id, count);
        return false;
    }

    sqlite3_prepare_v2(db, "DELETE FROM spell_templates WHERE id=?", -1, &stmt, nullptr);
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
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted spell %d.", id);
    return true;
}

// ---------------------------------------------------------------------------
// DrawFields
// ---------------------------------------------------------------------------

static bool DrawFields(SpellTemplate& t) {
    bool changed = false;

    char buf[128];
    std::strncpy(buf, t.name.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    if (ImGui::InputText("Name", buf, sizeof(buf))) { t.name = buf; changed = true; }

    if (ImGui::Combo("Type", &t.spell_type, kSpellTypes, 4)) changed = true;

    // Damage/debuff fields
    if (t.spell_type == 0 || t.spell_type == 3) {
        if (ImGui::InputInt("Damage Min", &t.damage_min)) changed = true;
        if (ImGui::InputInt("Damage Max", &t.damage_max)) changed = true;
        if (t.damage_min < 0)            t.damage_min = 0;
        if (t.damage_max < t.damage_min) t.damage_max = t.damage_min;
    } else {
        t.damage_min = t.damage_max = 0;
    }

    if (ImGui::InputInt("EP Cost",       &t.ep_cost))     changed = true;
    if (ImGui::InputInt("Cooldown (ms)", &t.cooldown_ms)) changed = true;
    if (ImGui::InputFloat("Range",       &t.range, 1.f, 5.f, "%.1f")) changed = true;
    if (ImGui::InputInt("Icon ID",       &t.icon))        changed = true;

    ImGui::Separator();
    ImGui::TextUnformatted("Area of Effect");
    ImGui::Spacing();

    if (ImGui::Combo("AoE Mode", &t.aoe_type, kAoETypes, 3)) changed = true;
    if (t.aoe_type > 0) {
        if (ImGui::InputFloat("AoE Radius", &t.aoe_radius, 0.5f, 2.f, "%.1f")) changed = true;
        if (t.aoe_radius < 0.f) t.aoe_radius = 0.f;
        ImGui::TextDisabled(t.aoe_type == 1
            ? "Hits all enemies within radius of the target."
            : "Player selects a point; hits all enemies within radius of that point.");
    } else {
        t.aoe_radius = 0.f;
    }

    if (t.ep_cost     < 0)   t.ep_cost     = 0;
    if (t.cooldown_ms < 0)   t.cooldown_ms = 0;
    if (t.range       < 0.f) t.range       = 0.f;
    if (t.icon        < 0)   t.icon        = 0;

    ImGui::Spacing();
    ImGui::TextDisabled("Spell script: scripts/server/spells/<name>.lua");

    return changed;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void SpellsTab::Draw(sqlite3* db) {
    if (needFetch_) { Fetch(db); needFetch_ = false; }

    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Spell")) { newSpell_ = {}; showNew_ = true; selected_ = -1; }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    float listW = 240.f;
    ImGui::BeginChild("##spell_list", {listW, 0}, true);
    for (int i = 0; i < (int)spells_.size(); ++i) {
        auto& s = spells_[i];
        const char* typeIcon = (s.spell_type == 0) ? "[D]"
                             : (s.spell_type == 1) ? "[H]"
                             : (s.spell_type == 2) ? "[B]" : "[X]"; // X = debuff
        const char* aoeIcon = (s.aoe_type == 1) ? " ⊕" : (s.aoe_type == 2) ? " ◎" : "";
        char label[160];
        std::snprintf(label, sizeof(label), "%s %s%s##sl%d", typeIcon, s.name.c_str(), aoeIcon, i);
        if (ImGui::Selectable(label, selected_ == i)) {
            selected_ = i;
            editing_  = s;
            dirty_    = false;
            showNew_  = false;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##spell_edit", {0, 0}, true);

    if (showNew_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Spell");
        ImGui::Separator();
        DrawFields(newSpell_);
        ImGui::Spacing();
        if (ImGui::Button("Create")) {
            if (Save(db, newSpell_)) showNew_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) showNew_ = false;

    } else if (selected_ >= 0 && selected_ < (int)spells_.size()) {
        ImGui::Text("Editing: [id=%d]  %s", editing_.id, editing_.name.c_str());
        ImGui::Separator();
        if (DrawFields(editing_)) dirty_ = true;
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Save"))   Save(db, editing_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) { editing_ = spells_[selected_]; dirty_ = false; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) Delete(db, editing_.id);
        ImGui::PopStyleColor();

    } else {
        ImGui::TextDisabled("Select a spell, or click \"New Spell\".");
    }

    ImGui::EndChild();
}

} // namespace gue
