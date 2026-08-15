#include "weapon_anim_styles.h"

#include <imgui.h>

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

void WeaponAnimStylesTab::SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_msg_, sizeof(status_msg_), fmt, args);
    va_end(args);
}

void WeaponAnimStylesTab::FetchStyles(sqlite3* db) {
    const int keep_id = editing_style_.id;

    styles_.clear();
    selected_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, style_key, display_name, description, CASE WHEN enabled THEN 1 ELSE 0 END "
        "FROM weapon_anim_styles ORDER BY style_key";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Style fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WeaponAnimStyleRow row;
        row.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) row.style_key = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 2)) row.display_name = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 3)) row.description = reinterpret_cast<const char*>(text);
        row.enabled = sqlite3_column_int(stmt, 4) != 0;
        styles_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (!styles_.empty()) {
        if (!select_style_key_after_fetch_.empty()) {
            for (int i = 0; i < static_cast<int>(styles_.size()); ++i) {
                if (styles_[i].style_key == select_style_key_after_fetch_) {
                    selected_ = i;
                    break;
                }
            }
        } else if (keep_id > 0) {
            for (int i = 0; i < static_cast<int>(styles_.size()); ++i) {
                if (styles_[i].id == keep_id) {
                    selected_ = i;
                    break;
                }
            }
        }
        if (selected_ >= 0 && selected_ < static_cast<int>(styles_.size())) {
            editing_style_ = styles_[selected_];
            dirty_style_ = false;
        }
    }

    select_style_key_after_fetch_.clear();
    SetStatus("Loaded %d weapon anim style(s).", static_cast<int>(styles_.size()));
}

bool WeaponAnimStylesTab::ValidateStyle(sqlite3* db,
                                        const WeaponAnimStyleRow& row,
                                        bool is_new,
                                        std::string* out_error) {
    if (TrimCopy(row.display_name).empty()) {
        if (out_error) *out_error = "Display name is required.";
        return false;
    }
    if (is_new) {
        if (row.style_key.empty()) {
            if (out_error) *out_error = "Style key is required.";
            return false;
        }
        if (row.style_key.size() > 32) {
            if (out_error) *out_error = "Style key must be 32 characters or less.";
            return false;
        }
        for (char c : row.style_key) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                if (out_error) *out_error = "Style key must contain only lowercase letters, digits, and underscores.";
                return false;
            }
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM weapon_anim_styles WHERE style_key=?", -1, &stmt, nullptr) != SQLITE_OK) {
            if (out_error) *out_error = "Failed to validate uniqueness.";
            return false;
        }
        sqlite3_bind_text(stmt, 1, row.style_key.c_str(), -1, SQLITE_TRANSIENT);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        if (count > 0) {
            if (out_error) *out_error = "Style key already exists.";
            return false;
        }
    }
    return true;
}

bool WeaponAnimStylesTab::DeleteStyleSoft(sqlite3* db, int style_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE weapon_anim_styles SET enabled=0 WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Failed to prepare style disable.");
        return false;
    }
    sqlite3_bind_int(stmt, 1, style_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Failed to disable style: %s", sqlite3_errmsg(db));
        return false;
    }
    SetStatus("Style disabled.");
    return true;
}

bool WeaponAnimStylesTab::SaveStyle(sqlite3* db) {
    std::string err;
    if (!ValidateStyle(db, editing_style_, false, &err)) {
        SetStatus("%s", err.c_str());
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql_upd = "UPDATE weapon_anim_styles SET display_name=?, description=?, enabled=? WHERE id=?";
    if (sqlite3_prepare_v2(db, sql_upd, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Failed to prepare UPDATE.");
        return false;
    }
    sqlite3_bind_text(stmt, 1, editing_style_.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, editing_style_.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, editing_style_.enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 4, editing_style_.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        SetStatus("Failed to update style.");
        return false;
    }
    sqlite3_finalize(stmt);

    SetStatus("Style saved.");
    dirty_style_ = false;
    return true;
}

void WeaponAnimStylesTab::DrawNewStyleForm(sqlite3* db) {
    ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Weapon Anim Style");
    ImGui::Separator();

    char buf_key[64] = {};
    char buf_name[64] = {};
    char buf_desc[512] = {};

    std::strncpy(buf_key, new_style_.style_key.c_str(), sizeof(buf_key) - 1);
    std::strncpy(buf_name, new_style_.display_name.c_str(), sizeof(buf_name) - 1);
    std::strncpy(buf_desc, new_style_.description.c_str(), sizeof(buf_desc) - 1);

    if (ImGui::InputText("Style Key", buf_key, sizeof(buf_key))) {
        new_style_.style_key = buf_key;
    }
    if (ImGui::InputText("Display Name", buf_name, sizeof(buf_name))) {
        new_style_.display_name = buf_name;
    }
    if (ImGui::InputTextMultiline("Description", buf_desc, sizeof(buf_desc), {-FLT_MIN, 60.0f})) {
        new_style_.description = buf_desc;
    }
    ImGui::Checkbox("Enabled", &new_style_.enabled);

    ImGui::Spacing();
    if (ImGui::Button("Create")) {
        std::string err;
        if (!ValidateStyle(db, new_style_, true, &err)) {
            SetStatus("%s", err.c_str());
            return;
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO weapon_anim_styles (style_key, display_name, description, enabled) VALUES (?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Create style prepare error: %s", sqlite3_errmsg(db));
            return;
        }

        sqlite3_bind_text(stmt, 1, new_style_.style_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, new_style_.display_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, new_style_.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, new_style_.enabled ? 1 : 0);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            SetStatus("Create style error: %s", sqlite3_errmsg(db));
            return;
        }

        select_style_key_after_fetch_ = new_style_.style_key;
        need_fetch_styles_ = true;
        show_new_form_ = false;
        SetStatus("Created style '%s'.", new_style_.style_key.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        show_new_form_ = false;
    }
}

void WeaponAnimStylesTab::DrawList(sqlite3* db) {
    const float list_width = 300.f;
    ImGui::BeginChild("##style_list", {list_width, 0.0f}, true);

    if (ImGui::Button("+ New Style")) {
        show_new_form_ = true;
        new_style_ = {};
        new_style_.enabled = true;
        selected_ = -1;
    }
    ImGui::Separator();

    for (int i = 0; i < static_cast<int>(styles_.size()); ++i) {
        const auto& s = styles_[i];
        char label[256];
        std::snprintf(label, sizeof(label), "%s%s##style_%d",
                      s.style_key.c_str(),
                      s.enabled ? "" : " (disabled)",
                      s.id);

        if (!s.enabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, {0.6f, 0.6f, 0.6f, 1.0f});
        }
        const bool sel = (selected_ == i);
        if (ImGui::Selectable(label, sel)) {
            selected_ = i;
            show_new_form_ = false;
            editing_style_ = styles_[i];
            dirty_style_ = false;
        }
        if (!s.enabled) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}

void WeaponAnimStylesTab::DrawEditor(sqlite3* db) {
    ImGui::SeparatorText("Identity");
    ImGui::Text("Style Key: %s", editing_style_.style_key.c_str());

    char buf_name[64] = {};
    std::strncpy(buf_name, editing_style_.display_name.c_str(), sizeof(buf_name) - 1);
    buf_name[sizeof(buf_name) - 1] = 0;
    if (ImGui::InputText("Display Name", buf_name, sizeof(buf_name))) {
        editing_style_.display_name = buf_name;
        dirty_style_ = true;
    }

    char buf_desc[512] = {};
    std::strncpy(buf_desc, editing_style_.description.c_str(), sizeof(buf_desc) - 1);
    buf_desc[sizeof(buf_desc) - 1] = 0;
    if (ImGui::InputTextMultiline("Description", buf_desc, sizeof(buf_desc), {-FLT_MIN, 60.0f})) {
        editing_style_.description = buf_desc;
        dirty_style_ = true;
    }

    if (ImGui::Checkbox("Enabled", &editing_style_.enabled)) {
        dirty_style_ = true;
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!dirty_style_);
    if (ImGui::Button("Save Style")) {
        if (SaveStyle(db)) {
            need_fetch_styles_ = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        if (selected_ >= 0 && selected_ < static_cast<int>(styles_.size())) {
            editing_style_ = styles_[selected_];
            dirty_style_ = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Style")) {
        ImGui::OpenPopup("Confirm Delete Style");
    }

    if (ImGui::BeginPopupModal("Confirm Delete Style", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Disable style '%s'?", editing_style_.style_key.c_str());
        ImGui::TextDisabled(
            "Items and Animation Vocabulary bindings referencing this style will\n"
            "keep their reference, but the style won't be offered in new combos\n"
            "and existing bindings using it will be flagged as dead until it's\n"
            "re-enabled.");
        ImGui::Separator();
        if (ImGui::Button("Disable", {120, 0})) {
            if (DeleteStyleSoft(db, editing_style_.id)) {
                need_fetch_styles_ = true;
                selected_ = -1;
                show_new_form_ = false;
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

void WeaponAnimStylesTab::Draw(sqlite3* db) {
    if (!db) return;

    if (need_fetch_styles_) {
        FetchStyles(db);
        need_fetch_styles_ = false;
    }

    if (ImGui::Button("Refresh")) {
        need_fetch_styles_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();
    ImGui::TextWrapped(
        "Weapon Anim Styles are the PHYSICAL grip/pose archetype a weapon uses "
        "(sword_1h, sword_2h, staff, bow, dagger, wand...) — independent of "
        "Weapon Kit (which skills it grants) and of Weapon Dimension/Hands on "
        "the item (damage and range mechanics). Many items with different "
        "kits, different stats, even different visuals can share one style: "
        "every 1-handed sword uses 'sword_1h' regardless of which skills or "
        "how much damage it has. This is what the Animation Vocabulary's "
        "'Add Weapon-Specific Binding' and the Actor Def preview's 'Simulate "
        "weapon equipped' both read from."
    );
    ImGui::Separator();

    DrawList(db);

    ImGui::SameLine();
    ImGui::BeginChild("##style_editor", {0.0f, 0.0f}, true);
    if (show_new_form_) {
        DrawNewStyleForm(db);
    } else if (selected_ >= 0 && selected_ < static_cast<int>(styles_.size())) {
        DrawEditor(db);
    } else {
        ImGui::TextDisabled("Select a style on the left or create a new one.");
    }
    ImGui::EndChild();
}

} // namespace gue
