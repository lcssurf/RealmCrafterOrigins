#include "weapon_kits.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gue {

namespace {

std::string TrimCopy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

} // namespace

void WeaponKitsTab::SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_msg_, sizeof(status_msg_), fmt, args);
    va_end(args);
}

void WeaponKitsTab::FetchKits(sqlite3* db) {
    const int keep_id = editing_kit_.id;

    kits_.clear();
    selected_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, kit_key, display_name, description, CASE WHEN enabled THEN 1 ELSE 0 END "
        "FROM weapon_kits ORDER BY kit_key";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Kit fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WeaponKitRow row;
        row.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) row.kit_key = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 2)) row.display_name = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 3)) row.description = reinterpret_cast<const char*>(text);
        row.enabled = sqlite3_column_int(stmt, 4) != 0;
        kits_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (!kits_.empty()) {
        if (!select_kit_key_after_fetch_.empty()) {
            for (int i = 0; i < static_cast<int>(kits_.size()); ++i) {
                if (kits_[i].kit_key == select_kit_key_after_fetch_) {
                    selected_ = i;
                    break;
                }
            }
        } else if (keep_id > 0) {
            for (int i = 0; i < static_cast<int>(kits_.size()); ++i) {
                if (kits_[i].id == keep_id) {
                    selected_ = i;
                    break;
                }
            }
        }
        if (selected_ >= 0 && selected_ < static_cast<int>(kits_.size())) {
            editing_kit_ = kits_[selected_];
            LoadKitAbilities(db, editing_kit_.id);
            dirty_kit_ = false;
            dirty_abilities_ = false;
        }
    } else {
        editing_abilities_.clear();
    }

    if (selected_ < 0) {
        editing_abilities_.clear();
    }
    select_kit_key_after_fetch_.clear();
    SetStatus("Loaded %d weapon kit(s).", static_cast<int>(kits_.size()));
}

void WeaponKitsTab::FetchAbilityOptions(sqlite3* db) {
    ability_options_.clear();

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, name "
        "FROM ability_templates "
        "WHERE enabled=1 "
        "ORDER BY name";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Ability options fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AbilityOption row;
        row.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) row.name = reinterpret_cast<const char*>(text);
        ability_options_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
}

void WeaponKitsTab::LoadKitAbilities(sqlite3* db, int kit_id) {
    editing_abilities_.clear();
    if (kit_id <= 0) return;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT ability_id, slot_index, CASE WHEN enabled THEN 1 ELSE 0 END "
        "FROM weapon_kit_abilities "
        "WHERE kit_id=? "
        "ORDER BY slot_index";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Kit abilities fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    sqlite3_bind_int(stmt, 1, kit_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WeaponKitAbilityRow row;
        row.ability_id = sqlite3_column_int(stmt, 0);
        row.slot_index = sqlite3_column_int(stmt, 1);
        row.enabled = sqlite3_column_int(stmt, 2) != 0;
        editing_abilities_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    for (int i = 0; i < static_cast<int>(editing_abilities_.size()); ++i) {
        editing_abilities_[i].slot_index = i;
    }
}

bool WeaponKitsTab::ValidateKit(sqlite3* db,
                                const WeaponKitRow& row,
                                bool is_new,
                                std::string* out_error) {
    if (TrimCopy(row.display_name).empty()) {
        if (out_error) *out_error = "Display name is required.";
        return false;
    }
    if (is_new) {
        if (row.kit_key.empty()) {
            if (out_error) *out_error = "Kit key is required.";
            return false;
        }
        if (row.kit_key.size() > 32) {
            if (out_error) *out_error = "Kit key must be 32 characters or less.";
            return false;
        }
        for (char c : row.kit_key) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                if (out_error) *out_error = "Kit key must contain only lowercase letters, digits, and underscores.";
                return false;
            }
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM weapon_kits WHERE kit_key=?", -1, &stmt, nullptr) != SQLITE_OK) {
            if (out_error) *out_error = "Failed to validate uniqueness.";
            return false;
        }
        sqlite3_bind_text(stmt, 1, row.kit_key.c_str(), -1, SQLITE_TRANSIENT);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        if (count > 0) {
            if (out_error) *out_error = "Kit key already exists.";
            return false;
        }
    }
    return true;
}

bool WeaponKitsTab::DeleteKitSoft(sqlite3* db, int kit_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE weapon_kits SET enabled=0 WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Failed to prepare kit disable.");
        return false;
    }
    sqlite3_bind_int(stmt, 1, kit_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Failed to disable kit: %s", sqlite3_errmsg(db));
        return false;
    }
    SetStatus("Kit disabled.");
    return true;
}

void WeaponKitsTab::DrawAbilitiesEditor() {
    ImGui::SeparatorText("Abilities");

    if (ability_options_.empty()) {
        ImGui::TextColored({1.0f, 0.7f, 0.4f, 1.0f},
            "No abilities defined yet. Create abilities in 'Combat Abilities' tab first.");
        return;
    }

    int remove_idx = -1;
    int swap_a = -1;
    int swap_b = -1;

    for (int i = 0; i < static_cast<int>(editing_abilities_.size()); ++i) {
        ImGui::PushID(i);
        auto& ab = editing_abilities_[i];

        ImGui::Text("Slot %d:", i);
        ImGui::SameLine();

        const char* current_name = "(none)";
        for (const auto& opt : ability_options_) {
            if (opt.id == ab.ability_id) {
                current_name = opt.name.c_str();
                break;
            }
        }

        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##ability", current_name)) {
            for (const auto& opt : ability_options_) {
                const bool selected = (opt.id == ab.ability_id);
                char label[256];
                std::snprintf(label, sizeof(label), "[%d] %s", opt.id, opt.name.c_str());
                if (ImGui::Selectable(label, selected)) {
                    ab.ability_id = opt.id;
                    dirty_abilities_ = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::Button("▲")) { swap_a = i; swap_b = i - 1; }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(i == static_cast<int>(editing_abilities_.size()) - 1);
        if (ImGui::Button("▼")) { swap_a = i; swap_b = i + 1; }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Checkbox("Enabled", &ab.enabled)) {
            dirty_abilities_ = true;
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            remove_idx = i;
        }
        ImGui::PopID();
    }

    if (swap_a >= 0 && swap_b >= 0) {
        std::swap(editing_abilities_[swap_a], editing_abilities_[swap_b]);
        for (int i = 0; i < static_cast<int>(editing_abilities_.size()); ++i) {
            editing_abilities_[i].slot_index = i;
        }
        dirty_abilities_ = true;
    }
    if (remove_idx >= 0) {
        editing_abilities_.erase(editing_abilities_.begin() + remove_idx);
        for (int i = 0; i < static_cast<int>(editing_abilities_.size()); ++i) {
            editing_abilities_[i].slot_index = i;
        }
        dirty_abilities_ = true;
    }

    if (ImGui::Button("+ Add Slot")) {
        WeaponKitAbilityRow row;
        row.slot_index = static_cast<int>(editing_abilities_.size());
        row.enabled = true;
        editing_abilities_.push_back(row);
        dirty_abilities_ = true;
    }
}

bool WeaponKitsTab::SaveKit(sqlite3* db) {
    std::string err;
    if (!ValidateKit(db, editing_kit_, false, &err)) {
        SetStatus("%s", err.c_str());
        return false;
    }

    for (int i = 0; i < static_cast<int>(editing_abilities_.size()); ++i) {
        if (editing_abilities_[i].ability_id <= 0) {
            SetStatus("Slot %d has no ability selected.", i);
            return false;
        }
    }
    for (int i = 0; i < static_cast<int>(editing_abilities_.size()); ++i) {
        for (int j = i + 1; j < static_cast<int>(editing_abilities_.size()); ++j) {
            if (editing_abilities_[i].ability_id == editing_abilities_[j].ability_id) {
                SetStatus("Ability %d appears in both slot %d and slot %d.",
                          editing_abilities_[i].ability_id, i, j);
                return false;
            }
        }
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql_upd = "UPDATE weapon_kits SET display_name=?, description=?, enabled=? WHERE id=?";
    if (sqlite3_prepare_v2(db, sql_upd, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Failed to prepare UPDATE.");
        return false;
    }
    sqlite3_bind_text(stmt, 1, editing_kit_.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, editing_kit_.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, editing_kit_.enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 4, editing_kit_.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        SetStatus("Failed to update kit.");
        return false;
    }
    sqlite3_finalize(stmt);

    if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        SetStatus("Failed to start ability transaction.");
        return false;
    }

    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM weapon_kit_abilities WHERE kit_id=?", -1, &del, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Failed to prepare ability cleanup.");
        return false;
    }
    sqlite3_bind_int(del, 1, editing_kit_.id);
    if (sqlite3_step(del) != SQLITE_DONE) {
        sqlite3_finalize(del);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Failed to clear old abilities.");
        return false;
    }
    sqlite3_finalize(del);

    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO weapon_kit_abilities (kit_id, ability_id, slot_index, enabled) VALUES (?, ?, ?, ?)", -1, &ins, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Failed to prepare ability insert.");
        return false;
    }

    bool ok = true;
    for (int i = 0; i < static_cast<int>(editing_abilities_.size()); ++i) {
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_int(ins, 1, editing_kit_.id);
        sqlite3_bind_int(ins, 2, editing_abilities_[i].ability_id);
        sqlite3_bind_int(ins, 3, i);
        sqlite3_bind_int(ins, 4, editing_abilities_[i].enabled ? 1 : 0);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(ins);

    if (ok) {
        sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        for (int i = 0; i < static_cast<int>(editing_abilities_.size()); ++i) {
            editing_abilities_[i].slot_index = i;
        }
        SetStatus("Kit saved.");
        dirty_kit_ = false;
        dirty_abilities_ = false;
        return true;
    }

    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    SetStatus("Failed to save abilities.");
    return false;
}

void WeaponKitsTab::DrawNewKitForm(sqlite3* db) {
    ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Weapon Kit");
    ImGui::Separator();

    char buf_key[64] = {};
    char buf_name[64] = {};
    char buf_desc[512] = {};

    std::strncpy(buf_key, new_kit_.kit_key.c_str(), sizeof(buf_key) - 1);
    std::strncpy(buf_name, new_kit_.display_name.c_str(), sizeof(buf_name) - 1);
    std::strncpy(buf_desc, new_kit_.description.c_str(), sizeof(buf_desc) - 1);

    if (ImGui::InputText("Kit Key", buf_key, sizeof(buf_key))) {
        new_kit_.kit_key = buf_key;
    }
    if (ImGui::InputText("Display Name", buf_name, sizeof(buf_name))) {
        new_kit_.display_name = buf_name;
    }
    if (ImGui::InputTextMultiline("Description", buf_desc, sizeof(buf_desc), {-FLT_MIN, 60.0f})) {
        new_kit_.description = buf_desc;
    }
    ImGui::Checkbox("Enabled", &new_kit_.enabled);

    ImGui::Spacing();
    if (ImGui::Button("Create")) {
        std::string err;
        if (!ValidateKit(db, new_kit_, true, &err)) {
            SetStatus("%s", err.c_str());
            return;
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO weapon_kits (kit_key, display_name, description, enabled) VALUES (?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Create kit prepare error: %s", sqlite3_errmsg(db));
            return;
        }

        sqlite3_bind_text(stmt, 1, new_kit_.kit_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, new_kit_.display_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, new_kit_.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, new_kit_.enabled ? 1 : 0);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            SetStatus("Create kit error: %s", sqlite3_errmsg(db));
            return;
        }

        select_kit_key_after_fetch_ = new_kit_.kit_key;
        need_fetch_kits_ = true;
        show_new_form_ = false;
        SetStatus("Created kit '%s'.", new_kit_.kit_key.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        show_new_form_ = false;
    }
}

void WeaponKitsTab::DrawList(sqlite3* db) {
    const float list_width = 360.f;
    ImGui::BeginChild("##kit_list", {list_width, 0.0f}, true);

    if (ImGui::Button("+ New Kit")) {
        show_new_form_ = true;
        new_kit_ = {};
        new_kit_.enabled = true;
        selected_ = -1;
    }
    ImGui::Separator();

    for (int i = 0; i < static_cast<int>(kits_.size()); ++i) {
        const auto& k = kits_[i];
        char label[256];
        std::snprintf(label, sizeof(label), "%s%s##kit_%d",
                      k.kit_key.c_str(),
                      k.enabled ? "" : " (disabled)",
                      k.id);

        if (!k.enabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, {0.6f, 0.6f, 0.6f, 1.0f});
        }
        const bool sel = (selected_ == i);
        if (ImGui::Selectable(label, sel)) {
            selected_ = i;
            show_new_form_ = false;
            editing_kit_ = kits_[i];
            LoadKitAbilities(db, kits_[i].id);
            dirty_kit_ = false;
            dirty_abilities_ = false;
        }
        if (!k.enabled) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}

void WeaponKitsTab::DrawEditor(sqlite3* db) {
    ImGui::SeparatorText("Identity");
    ImGui::Text("Kit Key:   %s", editing_kit_.kit_key.c_str());

    char buf_name[64] = {};
    std::strncpy(buf_name, editing_kit_.display_name.c_str(), sizeof(buf_name) - 1);
    buf_name[sizeof(buf_name) - 1] = 0;
    if (ImGui::InputText("Display Name", buf_name, sizeof(buf_name))) {
        editing_kit_.display_name = buf_name;
        dirty_kit_ = true;
    }

    char buf_desc[512] = {};
    std::strncpy(buf_desc, editing_kit_.description.c_str(), sizeof(buf_desc) - 1);
    buf_desc[sizeof(buf_desc) - 1] = 0;
    if (ImGui::InputTextMultiline("Description", buf_desc, sizeof(buf_desc), {-FLT_MIN, 60.0f})) {
        editing_kit_.description = buf_desc;
        dirty_kit_ = true;
    }

    if (ImGui::Checkbox("Enabled", &editing_kit_.enabled)) {
        dirty_kit_ = true;
    }

    DrawAbilitiesEditor();

    ImGui::Separator();
    ImGui::BeginDisabled(!dirty_kit_ && !dirty_abilities_);
    if (ImGui::Button("Save Kit")) {
        if (SaveKit(db)) {
            need_fetch_kits_ = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        if (selected_ >= 0 && selected_ < static_cast<int>(kits_.size())) {
            editing_kit_ = kits_[selected_];
            LoadKitAbilities(db, editing_kit_.id);
            dirty_kit_ = false;
            dirty_abilities_ = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Kit")) {
        ImGui::OpenPopup("Confirm Delete");
    }

    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Disable kit '%s'?", editing_kit_.kit_key.c_str());
        ImGui::TextDisabled("Items referencing this kit will keep their reference,\nbut won't get skills until kit is re-enabled.");
        ImGui::Separator();
        if (ImGui::Button("Disable", {120, 0})) {
            if (DeleteKitSoft(db, editing_kit_.id)) {
                need_fetch_kits_ = true;
                selected_ = -1;
                show_new_form_ = false;
                editing_abilities_.clear();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void WeaponKitsTab::Draw(sqlite3* db) {
    if (!db) return;

    if (need_fetch_kits_) {
        FetchKits(db);
        need_fetch_kits_ = false;
    }
    if (need_fetch_abilities_) {
        FetchAbilityOptions(db);
        need_fetch_abilities_ = false;
    }

    if (ImGui::Button("Refresh")) {
        need_fetch_kits_ = true;
        need_fetch_abilities_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();

    DrawList(db);

    ImGui::SameLine();
    ImGui::BeginChild("##kit_editor", {0.0f, 0.0f}, true);
    if (show_new_form_) {
        DrawNewKitForm(db);
    } else if (selected_ >= 0 && selected_ < static_cast<int>(kits_.size())) {
        DrawEditor(db);
    } else {
        ImGui::TextDisabled("Select a kit on the left or create a new one.");
    }
    ImGui::EndChild();
}

} // namespace gue
