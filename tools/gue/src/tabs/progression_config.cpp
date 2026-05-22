#include "progression_config.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace gue {

namespace {

std::string NormalizeCurveType(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "irregular" || value == "linear" || value == "quadratic" || value == "exponential") {
        return value;
    }
    return "irregular";
}

int64_t ComputeThreshold(int level, const std::string& curve_type, int base,
                         float exponent, float irregularity) {
    if (level <= 1) return 0;
    if (base <= 0) base = 1;
    if (exponent <= 0.0f) exponent = 1.0f;
    irregularity = std::clamp(irregularity, 0.0f, 1.0f);

    const std::string curve = NormalizeCurveType(curve_type);
    const double l = static_cast<double>(level);
    if (curve == "irregular") {
        const double base_value = static_cast<double>(base) * std::pow(l, static_cast<double>(exponent));
        const double jitter = (static_cast<double>((level * 73 + 37) % 100) / 100.0) *
                              static_cast<double>(irregularity);
        return static_cast<int64_t>(std::llround(base_value * (1.0 + jitter)));
    }
    if (curve == "exponential") {
        return static_cast<int64_t>(std::llround(static_cast<double>(base) * std::pow(1.5, l - 1.0)));
    }
    if (curve == "quadratic") {
        const int64_t n = static_cast<int64_t>(level - 1);
        return static_cast<int64_t>(base) * n * n;
    }
    return static_cast<int64_t>(base) * static_cast<int64_t>(level - 1);
}

bool HasColumn(sqlite3* db, const char* table, const char* column) {
    char sql[256];
    std::snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name != nullptr && std::strcmp(name, column) == 0) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

void AddColumnIfMissing(sqlite3* db, const char* table, const char* column, const char* column_sql) {
    if (HasColumn(db, table, column)) return;
    char sql[512];
    std::snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s", table, column_sql);
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

} // namespace

void ProgressionConfigTab::SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_msg_, sizeof(status_msg_), fmt, args);
    va_end(args);
}

void ProgressionConfigTab::EnsureTables(sqlite3* db) {
    if (tables_ensured_ || !db) return;

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS character_progression_config ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "max_level INTEGER NOT NULL DEFAULT 60,"
        "xp_curve_type TEXT NOT NULL DEFAULT 'irregular',"
        "xp_curve_base INTEGER NOT NULL DEFAULT 60,"
        "xp_curve_factor REAL NOT NULL DEFAULT 1.3,"
        "xp_curve_exponent REAL NOT NULL DEFAULT 2.5,"
        "xp_irregularity REAL NOT NULL DEFAULT 0.4,"
        "stat_points_per_level INTEGER NOT NULL DEFAULT 5,"
        "initial_stat_value INTEGER NOT NULL DEFAULT 5,"
        "respec_free_until_level INTEGER NOT NULL DEFAULT 10,"
        "respec_cost_gold INTEGER NOT NULL DEFAULT 1000"
        ")",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS skill_progression_config ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "xp_per_use INTEGER NOT NULL DEFAULT 10,"
        "max_level INTEGER NOT NULL DEFAULT 10,"
        "xp_curve_type TEXT NOT NULL DEFAULT 'irregular',"
        "xp_curve_base INTEGER NOT NULL DEFAULT 40,"
        "xp_curve_exponent REAL NOT NULL DEFAULT 2.0,"
        "xp_irregularity REAL NOT NULL DEFAULT 0.5,"
        "damage_bonus_per_level REAL NOT NULL DEFAULT 0.03,"
        "cooldown_redux_per_level REAL NOT NULL DEFAULT 0.01"
        ")",
        nullptr, nullptr, nullptr);

    AddColumnIfMissing(db, "character_progression_config", "xp_curve_exponent",
                       "xp_curve_exponent REAL NOT NULL DEFAULT 2.5");
    AddColumnIfMissing(db, "character_progression_config", "xp_irregularity",
                       "xp_irregularity REAL NOT NULL DEFAULT 0.4");
    AddColumnIfMissing(db, "character_progression_config", "xp_curve_factor",
                       "xp_curve_factor REAL NOT NULL DEFAULT 1.3");
    AddColumnIfMissing(db, "character_progression_config", "stat_points_per_level",
                       "stat_points_per_level INTEGER NOT NULL DEFAULT 5");
    AddColumnIfMissing(db, "character_progression_config", "initial_stat_value",
                       "initial_stat_value INTEGER NOT NULL DEFAULT 5");
    AddColumnIfMissing(db, "character_progression_config", "respec_free_until_level",
                       "respec_free_until_level INTEGER NOT NULL DEFAULT 10");
    AddColumnIfMissing(db, "character_progression_config", "respec_cost_gold",
                       "respec_cost_gold INTEGER NOT NULL DEFAULT 1000");
    AddColumnIfMissing(db, "skill_progression_config", "xp_curve_exponent",
                       "xp_curve_exponent REAL NOT NULL DEFAULT 2.0");
    AddColumnIfMissing(db, "skill_progression_config", "xp_irregularity",
                       "xp_irregularity REAL NOT NULL DEFAULT 0.5");

    sqlite3_exec(db,
        "INSERT OR IGNORE INTO character_progression_config "
        "(id, max_level, xp_curve_type, xp_curve_base, xp_curve_factor, xp_curve_exponent, xp_irregularity,"
        " stat_points_per_level, initial_stat_value, respec_free_until_level, respec_cost_gold) "
        "VALUES (1, 60, 'irregular', 60, 1.3, 2.5, 0.4, 5, 5, 10, 1000)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "INSERT OR IGNORE INTO skill_progression_config "
        "(id, xp_per_use, max_level, xp_curve_type, xp_curve_base, xp_curve_exponent, xp_irregularity, "
        " damage_bonus_per_level, cooldown_redux_per_level) "
        "VALUES (1, 10, 10, 'irregular', 40, 2.0, 0.5, 0.03, 0.01)",
        nullptr, nullptr, nullptr);

    tables_ensured_ = true;
}

void ProgressionConfigTab::Load(sqlite3* db) {
    if (loaded_ || !db) return;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT max_level, xp_curve_type, xp_curve_base, xp_curve_factor, xp_curve_exponent, xp_irregularity,"
        "       stat_points_per_level, initial_stat_value, respec_free_until_level, respec_cost_gold "
        "FROM character_progression_config WHERE id=1",
        -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            character_.max_level = sqlite3_column_int(stmt, 0);
            if (const auto* text = sqlite3_column_text(stmt, 1)) {
                character_.xp_curve_type = reinterpret_cast<const char*>(text);
            }
            character_.xp_curve_base = sqlite3_column_int(stmt, 2);
            character_.xp_curve_factor = static_cast<float>(sqlite3_column_double(stmt, 3));
            character_.xp_curve_exponent = static_cast<float>(sqlite3_column_double(stmt, 4));
            character_.xp_irregularity = static_cast<float>(sqlite3_column_double(stmt, 5));
            character_.stat_points_per_level = sqlite3_column_int(stmt, 6);
            character_.initial_stat_value = sqlite3_column_int(stmt, 7);
            character_.respec_free_until_level = sqlite3_column_int(stmt, 8);
            character_.respec_cost_gold = sqlite3_column_int(stmt, 9);
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT xp_per_use, max_level, xp_curve_type, xp_curve_base, xp_curve_exponent, xp_irregularity, "
        "       damage_bonus_per_level, cooldown_redux_per_level "
        "FROM skill_progression_config WHERE id=1",
        -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            mastery_.xp_per_use = sqlite3_column_int(stmt, 0);
            mastery_.max_level = sqlite3_column_int(stmt, 1);
            if (const auto* text = sqlite3_column_text(stmt, 2)) {
                mastery_.xp_curve_type = reinterpret_cast<const char*>(text);
            }
            mastery_.xp_curve_base = sqlite3_column_int(stmt, 3);
            mastery_.xp_curve_exponent = static_cast<float>(sqlite3_column_double(stmt, 4));
            mastery_.xp_irregularity = static_cast<float>(sqlite3_column_double(stmt, 5));
            mastery_.damage_bonus_per_level = static_cast<float>(sqlite3_column_double(stmt, 6));
            mastery_.cooldown_redux_per_level = static_cast<float>(sqlite3_column_double(stmt, 7));
        }
    }
    sqlite3_finalize(stmt);

    character_.xp_curve_type = NormalizeCurveType(character_.xp_curve_type);
    mastery_.xp_curve_type = NormalizeCurveType(mastery_.xp_curve_type);
    loaded_ = true;
}

bool ProgressionConfigTab::DrawCurveEditor(const char* id_prefix, std::string& curve_type, int& base,
                                           float& exponent, float& irregularity, int& max_level) {
    bool changed = false;
    static const char* kCurves[] = {"irregular", "linear", "quadratic", "exponential"};

    int curve_idx = 0;
    for (int i = 0; i < 4; ++i) {
        if (curve_type == kCurves[i]) {
            curve_idx = i;
            break;
        }
    }

    ImGui::PushID(id_prefix);
    if (ImGui::Combo("Curve Type", &curve_idx, kCurves, 4)) {
        curve_type = kCurves[curve_idx];
        changed = true;
    }
    if (ImGui::InputInt("Max Level", &max_level)) changed = true;
    if (ImGui::InputInt("Curve Base", &base)) changed = true;
    if (ImGui::InputFloat("Curve Exponent", &exponent, 0.1f, 0.5f, "%.2f")) changed = true;
    if (ImGui::InputFloat("Irregularity", &irregularity, 0.05f, 0.2f, "%.2f")) changed = true;
    ImGui::PopID();

    if (max_level < 1) max_level = 1;
    if (base < 1) base = 1;
    if (exponent <= 0.f) exponent = 1.f;
    irregularity = std::clamp(irregularity, 0.0f, 1.0f);
    curve_type = NormalizeCurveType(curve_type);
    return changed;
}

void ProgressionConfigTab::DrawPreview(const char* label, const std::string& curve_type, int base,
                                       float exponent, float irregularity) {
    static const int kPreviewLevels[] = {1, 5, 10, 20, 30, 40, 50, 60};
    ImGui::TextDisabled("%s", label);
    if (ImGui::BeginTable(label, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Cumulative XP");
        ImGui::TableHeadersRow();
        for (int level : kPreviewLevels) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", level);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%lld", static_cast<long long>(
                ComputeThreshold(level, curve_type, base, exponent, irregularity)));
        }
        ImGui::EndTable();
    }
}

void ProgressionConfigTab::SaveSettings(sqlite3* db) {
    if (!db) return;

    character_.xp_curve_type = NormalizeCurveType(character_.xp_curve_type);
    mastery_.xp_curve_type = NormalizeCurveType(mastery_.xp_curve_type);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "UPDATE character_progression_config "
        "SET max_level=?, xp_curve_type=?, xp_curve_base=?, xp_curve_factor=?, "
        "    xp_curve_exponent=?, xp_irregularity=?, "
        "    stat_points_per_level=?, initial_stat_value=?, respec_free_until_level=?, respec_cost_gold=? "
        "WHERE id=1",
        -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Character config save error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, character_.max_level);
    sqlite3_bind_text(stmt, 2, character_.xp_curve_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, character_.xp_curve_base);
    sqlite3_bind_double(stmt, 4, character_.xp_curve_factor);
    sqlite3_bind_double(stmt, 5, character_.xp_curve_exponent);
    sqlite3_bind_double(stmt, 6, character_.xp_irregularity);
    sqlite3_bind_int(stmt, 7, character_.stat_points_per_level);
    sqlite3_bind_int(stmt, 8, character_.initial_stat_value);
    sqlite3_bind_int(stmt, 9, character_.respec_free_until_level);
    sqlite3_bind_int(stmt, 10, character_.respec_cost_gold);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Character config save error: %s", sqlite3_errmsg(db));
        return;
    }

    stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "UPDATE skill_progression_config "
        "SET xp_per_use=?, max_level=?, xp_curve_type=?, xp_curve_base=?, "
        "    xp_curve_exponent=?, xp_irregularity=?, damage_bonus_per_level=?, cooldown_redux_per_level=? "
        "WHERE id=1",
        -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Mastery config save error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, mastery_.xp_per_use);
    sqlite3_bind_int(stmt, 2, mastery_.max_level);
    sqlite3_bind_text(stmt, 3, mastery_.xp_curve_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, mastery_.xp_curve_base);
    sqlite3_bind_double(stmt, 5, mastery_.xp_curve_exponent);
    sqlite3_bind_double(stmt, 6, mastery_.xp_irregularity);
    sqlite3_bind_double(stmt, 7, mastery_.damage_bonus_per_level);
    sqlite3_bind_double(stmt, 8, mastery_.cooldown_redux_per_level);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Mastery config save error: %s", sqlite3_errmsg(db));
        return;
    }

    dirty_ = false;
    SetStatus("Progression settings saved. Restart server to reload runtime caches.");
}

void ProgressionConfigTab::Draw(sqlite3* db) {
    EnsureTables(db);
    Load(db);

    ImGui::TextWrapped("Configure global character XP and default skill mastery curves. "
                       "Character settings are loaded into the server cache on startup. "
                       "Mastery settings are templates for skill defaults; individual abilities can override them.");
    if (status_msg_[0] != '\0') {
        ImGui::TextDisabled("%s", status_msg_);
    }
    ImGui::Separator();

    if (ImGui::BeginChild("##progression_config_body", {0.f, -34.f}, false)) {
        ImGui::TextUnformatted("Character Progression");
        if (DrawCurveEditor("char", character_.xp_curve_type, character_.xp_curve_base,
                            character_.xp_curve_exponent, character_.xp_irregularity,
                            character_.max_level)) {
            dirty_ = true;
        }
        if (ImGui::InputInt("Stat Points per Level", &character_.stat_points_per_level)) dirty_ = true;
        if (ImGui::InputInt("Initial Stat Value", &character_.initial_stat_value)) dirty_ = true;
        if (ImGui::InputInt("Respec Free Until Level", &character_.respec_free_until_level)) dirty_ = true;
        if (ImGui::InputInt("Respec Cost Gold", &character_.respec_cost_gold)) dirty_ = true;
        if (character_.stat_points_per_level < 0) character_.stat_points_per_level = 0;
        if (character_.initial_stat_value < 1) character_.initial_stat_value = 1;
        if (character_.respec_free_until_level < 0) character_.respec_free_until_level = 0;
        if (character_.respec_cost_gold < 0) character_.respec_cost_gold = 0;
        ImGui::Spacing();
        DrawPreview("Character Threshold Preview", character_.xp_curve_type, character_.xp_curve_base,
                    character_.xp_curve_exponent, character_.xp_irregularity);

        ImGui::Separator();
        ImGui::TextUnformatted("Mastery Progression Defaults");
        if (ImGui::InputInt("XP per Skill Use", &mastery_.xp_per_use)) dirty_ = true;
        if (mastery_.xp_per_use < 1) mastery_.xp_per_use = 1;
        if (DrawCurveEditor("mastery", mastery_.xp_curve_type, mastery_.xp_curve_base,
                            mastery_.xp_curve_exponent, mastery_.xp_irregularity,
                            mastery_.max_level)) {
            dirty_ = true;
        }
        if (ImGui::InputFloat("Damage Bonus per Level", &mastery_.damage_bonus_per_level, 0.005f, 0.02f, "%.3f")) {
            dirty_ = true;
        }
        if (ImGui::InputFloat("Cooldown Reduction per Level", &mastery_.cooldown_redux_per_level, 0.005f, 0.02f, "%.3f")) {
            dirty_ = true;
        }
        mastery_.damage_bonus_per_level = std::clamp(mastery_.damage_bonus_per_level, 0.0f, 1.0f);
        mastery_.cooldown_redux_per_level = std::clamp(mastery_.cooldown_redux_per_level, 0.0f, 1.0f);

        ImGui::Spacing();
        DrawPreview("Mastery Threshold Preview", mastery_.xp_curve_type, mastery_.xp_curve_base,
                    mastery_.xp_curve_exponent, mastery_.xp_irregularity);
    }
    ImGui::EndChild();

    if (ImGui::Button("Save Settings", {140.f, 0.f})) {
        SaveSettings(db);
    }
    ImGui::SameLine();
    if (!dirty_) {
        ImGui::TextDisabled("No unsaved changes.");
    } else {
        ImGui::TextColored({0.9f, 0.75f, 0.25f, 1.f}, "Unsaved changes");
    }
}

} // namespace gue
