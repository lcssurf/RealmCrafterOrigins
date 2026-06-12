#include "settings.h"

#include <imgui.h>
#include "../ui_widgets.h"

#include <cstdio>
#include <string>

namespace gue {

namespace {

static std::string ColText(sqlite3_stmt* stmt, int col) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? std::string(text) : std::string();
}

static std::string BuildModelLabel(int id, const std::string& name, const std::string& filePath) {
    std::string label = std::to_string(id) + ": ";
    if (!name.empty()) {
        label += name;
    } else {
        label += "(unnamed)";
    }
    if (!filePath.empty()) {
        label += " [" + filePath + "]";
    }
    return label;
}

static bool LoadSetting(sqlite3* db, const char* key, std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT value FROM game_settings WHERE key = ? LIMIT 1",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = ColText(stmt, 0);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool SaveSetting(sqlite3* db, const char* key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO game_settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

} // namespace

void SettingsTab::EnsureTables(sqlite3* db) {
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS game_settings ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL DEFAULT ''"
        ")",
        nullptr, nullptr, nullptr);
}

void SettingsTab::LoadModels(sqlite3* db) {
    media_models_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, file_path FROM media_models ORDER BY id",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_), "Load models: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int id = sqlite3_column_int(stmt, 0);
        media_models_.push_back({
            id,
            BuildModelLabel(id, ColText(stmt, 1), ColText(stmt, 2))
        });
    }
    sqlite3_finalize(stmt);

    std::snprintf(status_msg_, sizeof(status_msg_),
                  "Loaded %d model(s).", (int)media_models_.size());
}

void SettingsTab::LoadBloodFXKeys(sqlite3* db) {
    blood_fx_keys_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT fx_key FROM fx_templates ORDER BY fx_key",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_), "Load blood FX keys: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string fxKey = ColText(stmt, 0);
        if (!fxKey.empty()) {
            blood_fx_keys_.push_back(fxKey);
        }
    }
    sqlite3_finalize(stmt);

    std::snprintf(status_msg_, sizeof(status_msg_),
                  "Loaded %d blood FX key(s).", (int)blood_fx_keys_.size());
}

void SettingsTab::LoadSettings(sqlite3* db) {
    default_drop_model_id_ = 0;
    default_drop_model_scale_ = 1.f;
    blood_fx_key_.clear();
    blood_fx_mode_ = "basic";

    std::string value;
    if (!LoadSetting(db, "default_drop_model_id", value)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Load settings: %s", sqlite3_errmsg(db));
    } else if (!value.empty()) {
        try {
            int parsed = std::stoi(value);
            if (parsed > 0) {
                default_drop_model_id_ = parsed;
            }
        } catch (...) {
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Invalid '%s' value in game_settings (using empty).",
                          "default_drop_model_id");
        }
    } else if (status_msg_[0] == '\0') {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Loaded default drop model: <empty>.");
    }

    if (!LoadSetting(db, "default_drop_model_scale", value)) {
        if (status_msg_[0] == '\0') {
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Load settings: %s", sqlite3_errmsg(db));
        }
    } else if (!value.empty()) {
        try {
            float parsed = std::stof(value);
            if (parsed < 0.01f) parsed = 0.01f;
            default_drop_model_scale_ = parsed;
        } catch (...) {
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Invalid '%s' value in game_settings (using default).",
                          "default_drop_model_scale");
        }
    }

    if (!LoadSetting(db, "blood_fx_key", value)) {
        if (status_msg_[0] == '\0') {
            blood_fx_key_.clear();
        }
    } else {
        blood_fx_key_ = value;
    }

    if (!LoadSetting(db, "blood_mode", value)) {
        if (status_msg_[0] == '\0') {
            blood_fx_mode_ = "basic";
        }
    } else {
        if (value == "all") {
            blood_fx_mode_ = "all";
        } else {
            blood_fx_mode_ = "basic";
        }
    }

    if (status_msg_[0] == '\0') {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Loaded settings.");
    }

    dirty_ = false;
}

bool SettingsTab::SaveSettings(sqlite3* db) {
    const std::string model_value = (default_drop_model_id_ == 0)
        ? std::string()
        : std::to_string(default_drop_model_id_);
    if (!SaveSetting(db, "default_drop_model_id", model_value)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        return false;
    }

    if (default_drop_model_scale_ < 0.01f) {
        default_drop_model_scale_ = 0.01f;
    }
    const std::string scale_value = std::to_string(default_drop_model_scale_);
    if (!SaveSetting(db, "default_drop_model_scale", scale_value)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        return false;
    }
    if (!SaveSetting(db, "blood_fx_key", blood_fx_key_)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        return false;
    }
    if (!SaveSetting(db, "blood_mode", blood_fx_mode_)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        return false;
    }

    dirty_ = false;
    std::snprintf(status_msg_, sizeof(status_msg_),
                  "Saved drop model id=%d, scale=%.2f, blood fx key=%s, mode=%s.",
                  default_drop_model_id_, default_drop_model_scale_, blood_fx_key_.c_str(), blood_fx_mode_.c_str());
    return true;
}

void SettingsTab::Draw(sqlite3* db) {
    if (!db) return;
    if (need_fetch_) {
        EnsureTables(db);
        LoadModels(db);
        LoadBloodFXKeys(db);
        LoadSettings(db);
        need_fetch_ = false;
    }

    ImGui::Text("Global settings");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        need_fetch_ = true;
        EnsureTables(db);
        LoadModels(db);
        LoadBloodFXKeys(db);
        LoadSettings(db);
        need_fetch_ = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();

    int current_id = default_drop_model_id_;
    std::string current_fx_key = blood_fx_key_;
    float current_scale = default_drop_model_scale_;

    ImGui::Text("Drop item mesh");
    if (media_models_.empty()) {
        ImGui::TextDisabled("No models found in media_models.");
    } else {
        if (ui::SearchableComboId("Default Drop Model", current_id, media_models_, "(none)")) {
            default_drop_model_id_ = current_id;
            dirty_ = true;
        }
    }
    if (ImGui::InputFloat("Drop Model Scale", &current_scale, 0.05f, 0.5f, "%.2f")) {
        default_drop_model_scale_ = current_scale;
        if (default_drop_model_scale_ < 0.01f) {
            default_drop_model_scale_ = 0.01f;
        }
        dirty_ = true;
    }
    ImGui::Spacing();
    ImGui::Text("Blood FX on hit");
    if (blood_fx_keys_.empty()) {
        ImGui::TextDisabled("No fx_keys found in fx_templates.");
    } else if (ui::SearchableComboString("Blood FX key", current_fx_key, blood_fx_keys_, "(none)")) {
        blood_fx_key_ = current_fx_key;
        dirty_ = true;
    }
    if (ImGui::RadioButton("On hit (basic only)", blood_fx_mode_ == "basic")) {
        if (blood_fx_mode_ != "basic") {
            blood_fx_mode_ = "basic";
            dirty_ = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("All hits", blood_fx_mode_ == "all")) {
        if (blood_fx_mode_ != "all") {
            blood_fx_mode_ = "all";
            dirty_ = true;
        }
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Value is stored as TEXT in game_settings: "
        "default_drop_model_id='' means no mesh; "
        "default_drop_model_scale is a float multiplier.");

    ImGui::Spacing();
    ImGui::BeginDisabled(!dirty_);
    if (ImGui::Button("Save")) {
        SaveSettings(db);
    }
    ImGui::EndDisabled();
}

} // namespace gue
