#include "fx_templates.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <glm/glm.hpp>

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

bool FXTemplatesTab::DrawFields(FXTemplateRow& row) {
    bool changed = false;

    char key_buf[128] = {};
    std::strncpy(key_buf, row.fx_key.c_str(), sizeof(key_buf) - 1);
    if (ImGui::InputText("FX Key", key_buf, sizeof(key_buf))) {
        row.fx_key = key_buf;
        changed = true;
    }

    char name_buf[128] = {};
    std::strncpy(name_buf, row.display_name.c_str(), sizeof(name_buf) - 1);
    if (ImGui::InputText("Display Name", name_buf, sizeof(name_buf))) {
        row.display_name = name_buf;
        changed = true;
    }

    ImGui::SeparatorText("Emission");
    if (ImGui::InputInt("Burst Count (0=stream)", &row.burst_count)) {
        if (row.burst_count < 0) row.burst_count = 0;
        changed = true;
    }
    if (ImGui::SliderFloat("Stream Interval (s)", &row.stream_interval, 0.01f, 0.5f, "%.3f")) changed = true;
    if (ImGui::SliderFloat("Lifetime (s)", &row.lifetime_seconds, 0.1f, 5.0f, "%.2f")) changed = true;

    ImGui::SeparatorText("Velocity");
    if (ImGui::SliderFloat("Speed Min", &row.speed_min, 0.0f, 10.0f, "%.2f")) changed = true;
    if (ImGui::SliderFloat("Speed Max", &row.speed_max, 0.0f, 10.0f, "%.2f")) changed = true;
    if (ImGui::InputFloat("Velocity Bias X", &row.velocity_bias_x)) changed = true;
    if (ImGui::InputFloat("Velocity Bias Y", &row.velocity_bias_y)) changed = true;
    if (ImGui::InputFloat("Velocity Bias Z", &row.velocity_bias_z)) changed = true;
    if (ImGui::SliderFloat("Velocity Spread", &row.velocity_spread, 0.0f, 3.15f, "%.3f")) changed = true;

    ImGui::SeparatorText("Visual");
    if (ImGui::ColorEdit4("Color Start", row.color_start)) changed = true;
    if (ImGui::ColorEdit4("Color End", row.color_end)) changed = true;
    if (ImGui::SliderFloat("Size Start", &row.size_start, 0.0f, 20.0f, "%.2f")) changed = true;
    if (ImGui::SliderFloat("Size End", &row.size_end, 0.0f, 20.0f, "%.2f")) changed = true;

    char tex_buf[256] = {};
    std::strncpy(tex_buf, row.texture_path.c_str(), sizeof(tex_buf) - 1);
    if (ImGui::InputText("Texture Path (empty=solid quad)", tex_buf, sizeof(tex_buf))) {
        row.texture_path = tex_buf;
        changed = true;
    }

    return changed;
}

rco::renderer::FXParams FXTemplatesTab::RowToFXParams(const FXTemplateRow& r) const {
    rco::renderer::FXParams p;
    p.burstCount = r.burst_count;
    p.streamInterval = r.stream_interval;
    p.lifetimeSeconds = r.lifetime_seconds;
    p.speedMin = r.speed_min;
    p.speedMax = r.speed_max;
    p.velBias = glm::vec3(r.velocity_bias_x, r.velocity_bias_y, r.velocity_bias_z);
    p.velSpread = r.velocity_spread;
    p.colorStart = glm::vec4(r.color_start[0], r.color_start[1], r.color_start[2], r.color_start[3]);
    p.colorEnd = glm::vec4(r.color_end[0], r.color_end[1], r.color_end[2], r.color_end[3]);
    p.sizeStart = r.size_start;
    p.sizeEnd = r.size_end;
    return p;
}

void FXTemplatesTab::SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_msg_, sizeof(status_msg_), fmt, args);
    va_end(args);
}

void FXTemplatesTab::FetchAll(sqlite3* db) {
    templates_.clear();
    selected_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, fx_key, display_name, burst_count, stream_interval, lifetime_seconds, "
        "       speed_min, speed_max, velocity_bias_x, velocity_bias_y, velocity_bias_z, velocity_spread, "
        "       color_start_r, color_start_g, color_start_b, color_start_a, "
        "       color_end_r, color_end_g, color_end_b, color_end_a, "
        "       size_start, size_end, texture_path, enabled "
        "FROM fx_templates ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("FX fetch prepare error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FXTemplateRow r;
        r.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) r.fx_key = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 2)) r.display_name = reinterpret_cast<const char*>(text);
        r.burst_count = sqlite3_column_int(stmt, 3);
        r.stream_interval = static_cast<float>(sqlite3_column_double(stmt, 4));
        r.lifetime_seconds = static_cast<float>(sqlite3_column_double(stmt, 5));
        r.speed_min = static_cast<float>(sqlite3_column_double(stmt, 6));
        r.speed_max = static_cast<float>(sqlite3_column_double(stmt, 7));
        r.velocity_bias_x = static_cast<float>(sqlite3_column_double(stmt, 8));
        r.velocity_bias_y = static_cast<float>(sqlite3_column_double(stmt, 9));
        r.velocity_bias_z = static_cast<float>(sqlite3_column_double(stmt, 10));
        r.velocity_spread = static_cast<float>(sqlite3_column_double(stmt, 11));
        r.color_start[0] = static_cast<float>(sqlite3_column_double(stmt, 12));
        r.color_start[1] = static_cast<float>(sqlite3_column_double(stmt, 13));
        r.color_start[2] = static_cast<float>(sqlite3_column_double(stmt, 14));
        r.color_start[3] = static_cast<float>(sqlite3_column_double(stmt, 15));
        r.color_end[0] = static_cast<float>(sqlite3_column_double(stmt, 16));
        r.color_end[1] = static_cast<float>(sqlite3_column_double(stmt, 17));
        r.color_end[2] = static_cast<float>(sqlite3_column_double(stmt, 18));
        r.color_end[3] = static_cast<float>(sqlite3_column_double(stmt, 19));
        r.size_start = static_cast<float>(sqlite3_column_double(stmt, 20));
        r.size_end = static_cast<float>(sqlite3_column_double(stmt, 21));
        if (const auto* text = sqlite3_column_text(stmt, 22)) r.texture_path = reinterpret_cast<const char*>(text);
        r.enabled = sqlite3_column_int(stmt, 23) != 0;
        templates_.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);

    need_fetch_ = false;
}

bool FXTemplatesTab::SaveTemplate(sqlite3* db, FXTemplateRow& row) {
    const std::string key = TrimCopy(row.fx_key);
    if (key.empty()) {
        SetStatus("FX Key cannot be empty.");
        return false;
    }
    row.fx_key = key;

    row.display_name = TrimCopy(row.display_name);

    if (row.burst_count < 0) row.burst_count = 0;
    if (row.stream_interval < 0.0f) row.stream_interval = 0.0f;
    if (row.lifetime_seconds < 0.0f) row.lifetime_seconds = 0.0f;
    if (row.speed_min < 0.0f) row.speed_min = 0.0f;
    if (row.speed_max < 0.0f) row.speed_max = 0.0f;
    if (row.velocity_spread < 0.0f) row.velocity_spread = 0.0f;
    if (row.size_start < 0.0f) row.size_start = 0.0f;
    if (row.size_end < 0.0f) row.size_end = 0.0f;

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    const bool is_new = (row.id == 0);

    if (is_new) {
        const char* sql =
            "INSERT INTO fx_templates ("
            "fx_key, display_name, burst_count, stream_interval, lifetime_seconds, "
            "speed_min, speed_max, velocity_bias_x, velocity_bias_y, velocity_bias_z, velocity_spread, "
            "color_start_r, color_start_g, color_start_b, color_start_a, "
            "color_end_r, color_end_g, color_end_b, color_end_a, "
            "size_start, size_end, texture_path, enabled"
            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("FX template create error: %s", sqlite3_errmsg(db));
            return false;
        }
    } else {
        const char* sql =
            "UPDATE fx_templates SET "
            "fx_key=?, display_name=?, burst_count=?, stream_interval=?, lifetime_seconds=?, "
            "speed_min=?, speed_max=?, velocity_bias_x=?, velocity_bias_y=?, velocity_bias_z=?, velocity_spread=?, "
            "color_start_r=?, color_start_g=?, color_start_b=?, color_start_a=?, "
            "color_end_r=?, color_end_g=?, color_end_b=?, color_end_a=?, "
            "size_start=?, size_end=?, texture_path=?, enabled=? "
            "WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("FX template save error: %s", sqlite3_errmsg(db));
            return false;
        }
    }

    sqlite3_bind_text(stmt, 1, row.fx_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, row.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, row.burst_count);
    sqlite3_bind_double(stmt, 4, row.stream_interval);
    sqlite3_bind_double(stmt, 5, row.lifetime_seconds);
    sqlite3_bind_double(stmt, 6, row.speed_min);
    sqlite3_bind_double(stmt, 7, row.speed_max);
    sqlite3_bind_double(stmt, 8, row.velocity_bias_x);
    sqlite3_bind_double(stmt, 9, row.velocity_bias_y);
    sqlite3_bind_double(stmt, 10, row.velocity_bias_z);
    sqlite3_bind_double(stmt, 11, row.velocity_spread);
    sqlite3_bind_double(stmt, 12, row.color_start[0]);
    sqlite3_bind_double(stmt, 13, row.color_start[1]);
    sqlite3_bind_double(stmt, 14, row.color_start[2]);
    sqlite3_bind_double(stmt, 15, row.color_start[3]);
    sqlite3_bind_double(stmt, 16, row.color_end[0]);
    sqlite3_bind_double(stmt, 17, row.color_end[1]);
    sqlite3_bind_double(stmt, 18, row.color_end[2]);
    sqlite3_bind_double(stmt, 19, row.color_end[3]);
    sqlite3_bind_double(stmt, 20, row.size_start);
    sqlite3_bind_double(stmt, 21, row.size_end);
    sqlite3_bind_text(stmt, 22, row.texture_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 23, row.enabled ? 1 : 0);
    if (!is_new) {
        sqlite3_bind_int(stmt, 24, row.id);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        SetStatus("FX template save error: %s", sqlite3_errmsg(db));
        return false;
    }

    if (is_new) {
        row.id = static_cast<int>(sqlite3_last_insert_rowid(db));
    }
    need_fetch_ = true;
    dirty_ = false;
    SetStatus("Saved FX template '%s' (id=%d).", row.fx_key.c_str(), row.id);
    return true;
}

bool FXTemplatesTab::DeleteTemplate(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE fx_templates SET enabled=0 WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("FX template delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("FX template delete error: %s", sqlite3_errmsg(db));
        return false;
    }

    need_fetch_ = true;
    selected_ = -1;
    SetStatus("Disabled FX template %d.", id);
    return true;
}

void FXTemplatesTab::Draw(sqlite3* db) {
    if (!db) return;
    if (need_fetch_) {
        FetchAll(db);
    }

    if (ImGui::Button("Refresh")) {
        need_fetch_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("New FX Template")) {
        show_new_ = true;
        show_new_preview_primed_ = false;
        new_row_ = {};
        selected_ = -1;
        dirty_ = false;
    }
    const float list_width = 420.f;
    ImGui::BeginChild("##fx_list", {list_width, 0.0f}, true);
    for (int i = 0; i < static_cast<int>(templates_.size()); ++i) {
        const auto& row = templates_[i];
        char label[512];
        std::snprintf(label, sizeof(label), "%s - %s%s",
                      row.fx_key.c_str(),
                      row.display_name.c_str(),
                      row.enabled ? "" : " [disabled]");
        if (ImGui::Selectable(label, selected_ == i)) {
            selected_ = i;
            editing_row_ = row;
            dirty_ = false;
            show_new_ = false;
        }
    }
    if (templates_.empty()) {
        ImGui::TextDisabled("No FX templates.");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##fx_editor", {0.0f, 0.0f}, true);
    const FXTemplateRow* active_row = nullptr;
    int active_id = -2;
    bool fields_changed = false;
    if (show_new_) {
        active_row = &new_row_;
        active_id = -1;
    } else if (selected_ >= 0 && selected_ < static_cast<int>(templates_.size())) {
        active_row = &editing_row_;
        active_id = editing_row_.id;
    }

    if (show_new_) {
        ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New FX Template");
        ImGui::Separator();
        fields_changed = DrawFields(new_row_);
        if (fields_changed) {
            dirty_ = true;
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Create")) {
            if (SaveTemplate(db, new_row_)) {
                show_new_ = false;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            show_new_ = false;
        }
    } else if (selected_ >= 0 && selected_ < static_cast<int>(templates_.size())) {
        ImGui::Text("Editing FX Template [id=%d]", editing_row_.id);
        ImGui::Separator();
        fields_changed = DrawFields(editing_row_);
        if (fields_changed) {
            dirty_ = true;
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Save")) {
            SaveTemplate(db, editing_row_);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Disable (soft delete)")) {
            DeleteTemplate(db, editing_row_.id);
        }
    } else {
        ImGui::TextDisabled("Select an FX template, or click \"New FX Template\".");
    }

    if (active_row) {
        bool should_update_preview = show_new_
            ? !show_new_preview_primed_
            : (active_id != preview_for_id_);
        if (fields_changed) {
            should_update_preview = true;
        }
        if (should_update_preview) {
            preview_.SetParams(RowToFXParams(*active_row));
            preview_for_id_ = active_id;
            show_new_preview_primed_ = true;
        }
        ImGui::SeparatorText("Preview");
        preview_.Draw({320.0f, 240.0f});
        ImGui::TextDisabled("Drag with left mouse to rotate | scroll to zoom");
    } else {
        show_new_preview_primed_ = false;
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%s", status_msg_);
}

} // namespace gue
