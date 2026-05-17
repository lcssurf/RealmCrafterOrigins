#include "equipment_slots.h"

#include <imgui.h>

#include <cstdarg>
#include <cstdio>

namespace gue {

namespace {

static const char* kSlotNames[10] = {
    "Weapon", "Shield", "Hat", "Chest", "Hands",
    "Belt", "Legs", "Feet", "Ring", "Amulet"
};

} // namespace

void EquipmentSlotsTab::SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_msg_, sizeof(status_msg_), fmt, args);
    va_end(args);
}

void EquipmentSlotsTab::Fetch(sqlite3* db) {
    for (int i = 0; i < 10; ++i) {
        rows_[i] = EquipmentSlotConfigRow{};
        rows_[i].slot_id = i;
        rows_[i].enabled = true;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT slot_id, "
        "       CASE WHEN gives_kit THEN 1 ELSE 0 END AS gives_kit, "
        "       hotbar_slots_granted, "
        "       CASE WHEN enabled THEN 1 ELSE 0 END AS enabled "
        "FROM equipment_slot_config "
        "ORDER BY slot_id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int slot_id = sqlite3_column_int(stmt, 0);
        if (slot_id < 0 || slot_id > 9) {
            continue;
        }
        auto& row = rows_[slot_id];
        row.slot_id = slot_id;
        row.gives_kit = sqlite3_column_int(stmt, 1) != 0;
        row.hotbar_slots_granted = sqlite3_column_int(stmt, 2);
        row.enabled = sqlite3_column_int(stmt, 3) != 0;
    }
    sqlite3_finalize(stmt);

    SetStatus("Loaded slot config.");
}

bool EquipmentSlotsTab::UpsertSlot(sqlite3* db, const EquipmentSlotConfigRow& row) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO equipment_slot_config "
        "(slot_id, gives_kit, hotbar_slots_granted, enabled) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(slot_id) DO UPDATE SET "
        "gives_kit = excluded.gives_kit, "
        "hotbar_slots_granted = excluded.hotbar_slots_granted, "
        "enabled = excluded.enabled";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Save prepare error: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(stmt, 1, row.slot_id);
    sqlite3_bind_int(stmt, 2, row.gives_kit ? 1 : 0);
    sqlite3_bind_int(stmt, 3, row.hotbar_slots_granted);
    sqlite3_bind_int(stmt, 4, row.enabled ? 1 : 0);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Save error: %s", sqlite3_errmsg(db));
        return false;
    }
    return true;
}

void EquipmentSlotsTab::Draw(sqlite3* db) {
    if (!db) return;

    if (need_fetch_) {
        Fetch(db);
        need_fetch_ = false;
    }

    if (ImGui::Button("Refresh")) {
        need_fetch_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();

    ImGui::TextWrapped(
        "Equipment Slots configure each equipment slot's contribution to the player's hotbar.");
    ImGui::TextWrapped(
        "  - Gives Kit: if checked, items equipped in this slot can grant skill kits.");
    ImGui::TextWrapped(
        "  - Hotbar Slots: how many hotbar slots this equipment grants to the player.");
    ImGui::TextWrapped(
        "Skills available come from the kit's pool (configured in 'Weapon Kits' tab). "
        "The player chooses which skills fill the granted hotbar slots from the pool.");
    ImGui::TextWrapped(
        "Default: weapon=4 slots, chest=1, feet=1 (will be applied via seed in next commit).");
    ImGui::Separator();

    if (ImGui::BeginTable("##equipment_slots_table", 5,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Slot");
        ImGui::TableSetupColumn("Gives Kit");
        ImGui::TableSetupColumn("Hotbar Slots");
        ImGui::TableSetupColumn("Enabled");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();

        for (int i = 0; i < 10; ++i) {
            auto& r = rows_[i];
            if (r.hotbar_slots_granted < 0) r.hotbar_slots_granted = 0;
            if (r.hotbar_slots_granted > 16) r.hotbar_slots_granted = 16;

            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("[%d] %s", i, kSlotNames[i]);

            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("##gives_kit", &r.gives_kit);

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputInt("##hotbar_slots", &r.hotbar_slots_granted)) {
                if (r.hotbar_slots_granted < 0) r.hotbar_slots_granted = 0;
                if (r.hotbar_slots_granted > 16) r.hotbar_slots_granted = 16;
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::Checkbox("##enabled", &r.enabled);

            ImGui::TableSetColumnIndex(4);
            if (ImGui::Button("Save")) {
                if (UpsertSlot(db, r)) {
                    SetStatus("Saved slot %d (%s).", i, kSlotNames[i]);
                }
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

} // namespace gue
