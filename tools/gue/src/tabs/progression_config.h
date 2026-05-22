#pragma once

#include <sqlite3.h>

#include <string>

namespace gue {

class ProgressionConfigTab {
public:
    void Draw(sqlite3* db);

private:
    struct CharacterConfig {
        int max_level = 60;
        std::string xp_curve_type = "irregular";
        int xp_curve_base = 60;
        float xp_curve_factor = 1.3f;
        float xp_curve_exponent = 2.5f;
        float xp_irregularity = 0.4f;
        int stat_points_per_level = 5;
        int initial_stat_value = 5;
        int respec_free_until_level = 10;
        int respec_cost_gold = 1000;
    };

    struct MasteryConfig {
        int xp_per_use = 10;
        int max_level = 10;
        std::string xp_curve_type = "irregular";
        int xp_curve_base = 40;
        float xp_curve_exponent = 2.0f;
        float xp_irregularity = 0.5f;
        float damage_bonus_per_level = 0.03f;
        float cooldown_redux_per_level = 0.01f;
    };

    void EnsureTables(sqlite3* db);
    void Load(sqlite3* db);
    void SaveSettings(sqlite3* db);
    bool DrawCurveEditor(const char* id_prefix, std::string& curve_type, int& base,
                         float& exponent, float& irregularity, int& max_level);
    void DrawPreview(const char* label, const std::string& curve_type, int base,
                     float exponent, float irregularity);
    void SetStatus(const char* fmt, ...);

    bool tables_ensured_ = false;
    bool loaded_ = false;
    bool dirty_ = false;
    CharacterConfig character_;
    MasteryConfig mastery_;
    char status_msg_[256] = {};
};

} // namespace gue
