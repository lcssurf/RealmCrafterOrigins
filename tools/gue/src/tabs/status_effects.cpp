#include "status_effects.h"

#include <imgui.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gue {

namespace {

std::string colText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? std::string(t) : std::string();
}

bool InputString(const char* id, std::string& s, size_t maxLen = 256) {
    std::vector<char> buf(maxLen);
    std::strncpy(buf.data(), s.c_str(), maxLen - 1);
    buf[maxLen - 1] = 0;
    if (ImGui::InputText(id, buf.data(), maxLen)) {
        s = buf.data();
        return true;
    }
    return false;
}

const char* kKinds[] = {"cc", "buff", "debuff"};
const char* kCCTypes[] = {"stun", "root", "silence", "slow"};
const char* kStackRules[] = {"refresh", "stack_up_to_max", "ignore_if_active"};

} // namespace

void StatusEffectsTab::SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_msg_, sizeof(status_msg_), fmt, args);
    va_end(args);
}

void StatusEffectsTab::EnsureTables(sqlite3* db) {
    if (tables_ensured_) return;
    tables_ensured_ = true;

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS status_effect_templates ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name        TEXT    NOT NULL DEFAULT '',"
        "  kind        TEXT    NOT NULL DEFAULT 'cc',"
        "  cc_type     TEXT    NOT NULL DEFAULT '',"
        "  duration_ms INTEGER NOT NULL DEFAULT 2000,"
        "  stack_rule  TEXT    NOT NULL DEFAULT 'refresh',"
        "  icon_path   TEXT    NOT NULL DEFAULT '',"
        "  enabled     INTEGER NOT NULL DEFAULT 1"
        ")",
        nullptr, nullptr, nullptr);

    // Additive migrations mirroring server/internal/db/db.go migrateV57 —
    // links an ability_templates row to a status_effect_templates row.
    // Safe no-ops when the column already exists.
    sqlite3_exec(db,
        "ALTER TABLE ability_templates ADD COLUMN status_effect_template_id INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE ability_templates ADD COLUMN cc_base_chance_pct REAL NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);

    // Additive migrations mirroring server/internal/db/db.go migrateV58 —
    // Fase 2, stat-modifier buffs/debuffs. Safe no-ops when already present.
    sqlite3_exec(db,
        "ALTER TABLE status_effect_templates ADD COLUMN stat_mods_json TEXT NOT NULL DEFAULT '[]'",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE status_effect_templates ADD COLUMN max_stacks INTEGER NOT NULL DEFAULT 1",
        nullptr, nullptr, nullptr);

    // Additive migrations mirroring server/internal/db/db.go migrateV59 —
    // Fase 3, DoT/HoT tick fields. Safe no-ops when already present.
    sqlite3_exec(db,
        "ALTER TABLE status_effect_templates ADD COLUMN tick_interval_ms INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE status_effect_templates ADD COLUMN tick_damage_min INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE status_effect_templates ADD COLUMN tick_damage_max INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE status_effect_templates ADD COLUMN is_heal INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
}

void StatusEffectsTab::FetchAll(sqlite3* db) {
    rows_.clear();
    need_fetch_ = false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, name, kind, cc_type, duration_ms, stack_rule, icon_path, enabled, "
        "stat_mods_json, max_stacks, tick_interval_ms, tick_damage_min, tick_damage_max, is_heal "
        "FROM status_effect_templates ORDER BY id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        StatusEffectTemplateRow row;
        row.id               = sqlite3_column_int(stmt, 0);
        row.name             = colText(stmt, 1);
        row.kind             = colText(stmt, 2);
        row.cc_type          = colText(stmt, 3);
        row.duration_ms      = sqlite3_column_int(stmt, 4);
        row.stack_rule       = colText(stmt, 5);
        row.icon_path        = colText(stmt, 6);
        row.enabled          = sqlite3_column_int(stmt, 7) != 0;
        row.stat_mods_json   = colText(stmt, 8);
        row.max_stacks       = sqlite3_column_int(stmt, 9);
        row.tick_interval_ms = sqlite3_column_int(stmt, 10);
        row.tick_damage_min  = sqlite3_column_int(stmt, 11);
        row.tick_damage_max  = sqlite3_column_int(stmt, 12);
        row.is_heal          = sqlite3_column_int(stmt, 13) != 0;
        rows_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
}

bool StatusEffectsTab::SaveRow(sqlite3* db, StatusEffectTemplateRow& row) {
    if (row.duration_ms < 0) row.duration_ms = 0;

    if (row.max_stacks < 1) row.max_stacks = 1;
    if (row.tick_interval_ms < 0) row.tick_interval_ms = 0;
    if (row.tick_damage_min < 0) row.tick_damage_min = 0;
    if (row.tick_damage_max < row.tick_damage_min) row.tick_damage_max = row.tick_damage_min;

    if (row.id == 0) {
        const char* sql =
            "INSERT INTO status_effect_templates "
            "(name, kind, cc_type, duration_ms, stack_rule, icon_path, enabled, "
            "stat_mods_json, max_stacks, tick_interval_ms, tick_damage_min, tick_damage_max, is_heal) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Insert prepare failed: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_text(stmt, 1, row.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, row.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, row.cc_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, row.duration_ms);
        sqlite3_bind_text(stmt, 5, row.stack_rule.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, row.icon_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, row.enabled ? 1 : 0);
        sqlite3_bind_text(stmt, 8, row.stat_mods_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 9, row.max_stacks);
        sqlite3_bind_int(stmt, 10, row.tick_interval_ms);
        sqlite3_bind_int(stmt, 11, row.tick_damage_min);
        sqlite3_bind_int(stmt, 12, row.tick_damage_max);
        sqlite3_bind_int(stmt, 13, row.is_heal ? 1 : 0);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (ok) {
            row.id = static_cast<int>(sqlite3_last_insert_rowid(db));
            SetStatus("Created '%s'", row.name.c_str());
        } else {
            SetStatus("Insert failed: %s", sqlite3_errmsg(db));
        }
        return ok;
    }

    const char* sql =
        "UPDATE status_effect_templates SET "
        "name=?, kind=?, cc_type=?, duration_ms=?, stack_rule=?, icon_path=?, enabled=?, "
        "stat_mods_json=?, max_stacks=?, tick_interval_ms=?, tick_damage_min=?, tick_damage_max=?, is_heal=? "
        "WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Update prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(stmt, 1, row.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, row.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, row.cc_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, row.duration_ms);
    sqlite3_bind_text(stmt, 5, row.stack_rule.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, row.icon_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, row.enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 8, row.stat_mods_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, row.max_stacks);
    sqlite3_bind_int(stmt, 10, row.tick_interval_ms);
    sqlite3_bind_int(stmt, 11, row.tick_damage_min);
    sqlite3_bind_int(stmt, 12, row.tick_damage_max);
    sqlite3_bind_int(stmt, 13, row.is_heal ? 1 : 0);
    sqlite3_bind_int(stmt, 14, row.id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    SetStatus(ok ? "Saved '%s'" : "Save failed: %s", ok ? row.name.c_str() : sqlite3_errmsg(db));
    return ok;
}

bool StatusEffectsTab::DeleteRow(sqlite3* db, int row_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM status_effect_templates WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, row_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok) SetStatus("Deleted #%d", row_id);
    return ok;
}

void StatusEffectsTab::Draw(sqlite3* db) {
    EnsureTables(db);
    if (need_fetch_) FetchAll(db);

    ImGui::TextDisabled(
        "CC: stun/root/silence/slow (cc_type). Buff/Debuff: stat_mods_json "
        "([{\"stat\":\"movement_speed_mult\",\"flat\":0,\"pct\":0.2}]), "
        "max_stacks only matters for stack_rule=stack_up_to_max. DoT/HoT: "
        "tick_interval_ms>0 (e.g. 1000=every 1s), tick_dmg_min/max rolled per "
        "tick, is_heal routes it through heal instead of damage.");

    if (ImGui::Button("Refresh")) need_fetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("+ Add")) {
        StatusEffectTemplateRow row;
        row.name = "New Status Effect";
        if (SaveRow(db, row)) need_fetch_ = true;
    }
    if (status_msg_[0]) {
        ImGui::SameLine();
        ImGui::TextUnformatted(status_msg_);
    }

    ImGui::Separator();

    if (ImGui::BeginTable("status_effect_templates", 15,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("CC Type", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("Duration (ms)", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("Stack Rule", ImGuiTableColumnFlags_WidthFixed, 130.f);
        ImGui::TableSetupColumn("Max Stacks", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("Stat Mods JSON", ImGuiTableColumnFlags_WidthFixed, 320.f);
        ImGui::TableSetupColumn("Tick Interval (ms)", ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupColumn("Tick Dmg Min", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Tick Dmg Max", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Is Heal", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Icon Path", ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableHeadersRow();

        for (auto& row : rows_) {
            ImGui::PushID(row.id);
            ImGui::TableNextRow();
            bool dirty = false;

            ImGui::TableNextColumn();
            ImGui::Text("%d", row.id);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            dirty |= InputString("##name", row.name);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##kind", row.kind.c_str())) {
                for (const char* k : kKinds) {
                    bool selected = row.kind == k;
                    if (ImGui::Selectable(k, selected)) {
                        row.kind = k;
                        dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            bool is_cc = row.kind == "cc";

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(!is_cc);
            if (ImGui::BeginCombo("##cctype", row.cc_type.c_str())) {
                for (const char* t : kCCTypes) {
                    bool selected = row.cc_type == t;
                    if (ImGui::Selectable(t, selected)) {
                        row.cc_type = t;
                        dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            dirty |= ImGui::InputInt("##duration", &row.duration_ms, 0);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##stackrule", row.stack_rule.c_str())) {
                for (const char* r : kStackRules) {
                    bool selected = row.stack_rule == r;
                    if (ImGui::Selectable(r, selected)) {
                        row.stack_rule = r;
                        dirty = true;
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(row.stack_rule != "stack_up_to_max");
            dirty |= ImGui::InputInt("##maxstacks", &row.max_stacks, 0);
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(is_cc);
            dirty |= InputString("##statmods", row.stat_mods_json, 512);
            ImGui::EndDisabled();

            // Fase 3 (DoT/HoT) — tick fields, only meaningful for buff/debuff.
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(is_cc);
            dirty |= ImGui::InputInt("##tickinterval", &row.tick_interval_ms, 0);
            ImGui::EndDisabled();

            bool has_tick = !is_cc && row.tick_interval_ms > 0;

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(!has_tick);
            dirty |= ImGui::InputInt("##tickdmgmin", &row.tick_damage_min, 0);
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(!has_tick);
            dirty |= ImGui::InputInt("##tickdmgmax", &row.tick_damage_max, 0);
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!has_tick);
            dirty |= ImGui::Checkbox("##isheal", &row.is_heal);
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            dirty |= InputString("##icon", row.icon_path);

            ImGui::TableNextColumn();
            dirty |= ImGui::Checkbox("##enabled", &row.enabled);

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Save")) dirty = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("Del")) {
                if (DeleteRow(db, row.id)) need_fetch_ = true;
            }

            if (dirty) SaveRow(db, row);

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

} // namespace gue
