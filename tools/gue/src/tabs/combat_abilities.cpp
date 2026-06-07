#include "combat_abilities.h"
#include "util/ability_json_validator.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gue {

namespace {

struct JsonTemplate {
    const char* name;
    const char* description;
    const char* json_content;
};

const JsonTemplate kDamageScaleTemplates[] = {
    {
        "Pure STR (warrior melee)",
        "Damage scales heavily with Strength",
        R"({
  "scaling": [
    {"stat": "STR", "coef": 1.5}
  ]
})",
    },
    {
        "Pure DEX (rogue/ranger)",
        "Damage scales with Dexterity",
        R"({
  "scaling": [
    {"stat": "DEX", "coef": 1.5}
  ]
})",
    },
    {
        "Pure INT (mage)",
        "Magic damage scales with Intelligence",
        R"({
  "scaling": [
    {"stat": "INT", "coef": 1.8}
  ]
})",
    },
    {
        "STR+DEX hybrid (hunter)",
        "Mixed physical scaling",
        R"({
  "scaling": [
    {"stat": "STR", "coef": 1.0},
    {"stat": "DEX", "coef": 0.7}
  ]
})",
    },
    {
        "STR+Level (progression scaling)",
        "Scales with stats and character level",
        R"({
  "scaling": [
    {"stat": "STR", "coef": 1.2},
    {"stat": "LEVEL", "coef": 0.5}
  ]
})",
    },
};

const JsonTemplate kCritPolicyTemplates[] = {
    {
        "Standard (5% base, DEX scaling)",
        "Vanilla MMO crit chance",
        R"({
  "base_chance_pct": 5.0,
  "scaling_stat": "DEX",
  "scaling_softcap_value": 1500,
  "scaling_softcap_pct": 0.70,
  "damage_multiplier": 1.5
})",
    },
    {
        "High crit (rogue-style)",
        "20% base, DEX scaling, 2.5x damage",
        R"({
  "base_chance_pct": 20.0,
  "scaling_stat": "DEX",
  "scaling_softcap_value": 1500,
  "scaling_softcap_pct": 0.80,
  "damage_multiplier": 2.5
})",
    },
    {
        "Heavy hitter (low chance, high mult)",
        "5% chance, 3x damage",
        R"({
  "base_chance_pct": 5.0,
  "scaling_stat": "STR",
  "scaling_softcap_value": 1500,
  "scaling_softcap_pct": 0.50,
  "damage_multiplier": 3.0
})",
    },
    {
        "Spell crit (INT-based)",
        "Magic crit scaling with Intelligence",
        R"({
  "base_chance_pct": 10.0,
  "scaling_stat": "INT",
  "scaling_softcap_value": 1500,
  "scaling_softcap_pct": 0.70,
  "damage_multiplier": 1.8
})",
    },
    {
        "Guaranteed crit (test/debug)",
        "100% crit chance, 2x damage",
        R"({
  "base_chance_pct": 100.0,
  "scaling_stat": "DEX",
  "scaling_softcap_value": 1500,
  "scaling_softcap_pct": 0.0,
  "damage_multiplier": 2.0
})",
    },
};

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

bool ExistsID(sqlite3* db, const char* sql, int id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count > 0;
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsAllowedCategory(const std::string& category) {
    static const char* kAllowed[] = {
        "damage", "heal", "buff", "debuff", "mobility", "utility", "summon"
    };
    const std::string normalized = ToLowerCopy(TrimCopy(category));
    for (const char* c : kAllowed) {
        if (normalized == c) return true;
    }
    return false;
}

bool IsAllowedCurveType(const std::string& curve_type) {
    static const char* kAllowed[] = {"irregular", "linear", "quadratic", "exponential"};
    const std::string normalized = ToLowerCopy(TrimCopy(curve_type));
    for (const char* c : kAllowed) {
        if (normalized == c) return true;
    }
    return false;
}

bool DrawAbilityFields(CombatAbilityTemplate& row) {
    bool changed = false;

    char name_buf[128] = {};
    std::strncpy(name_buf, row.name.c_str(), sizeof(name_buf) - 1);
    if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
        row.name = name_buf;
        changed = true;
    }

    ImGui::TextUnformatted("Description");
    char description_buf[2048] = {};
    std::strncpy(description_buf, row.description.c_str(), sizeof(description_buf) - 1);
    if (ImGui::InputTextMultiline("##ability_description", description_buf, sizeof(description_buf), {-1.0f, 64.0f})) {
        row.description = description_buf;
        changed = true;
    }

    char family_buf[64] = {};
    std::strncpy(family_buf, row.family.c_str(), sizeof(family_buf) - 1);
    if (ImGui::InputText("Family", family_buf, sizeof(family_buf))) {
        row.family = family_buf;
        changed = true;
    }

    static const char* kResourceTypes[] = {"none", "sp", "mp", "hp"};
    int resource_idx = 0;
    for (int i = 0; i < 4; ++i) {
        if (row.resource_type == kResourceTypes[i]) {
            resource_idx = i;
            break;
        }
    }
    if (ImGui::Combo("Resource Type", &resource_idx, kResourceTypes, 4)) {
        row.resource_type = kResourceTypes[resource_idx];
        changed = true;
    }

    if (ImGui::InputInt("Resource Cost", &row.resource_cost)) changed = true;
    if (ImGui::InputInt("Cooldown (ms)", &row.cooldown_ms)) changed = true;
    if (ImGui::InputFloat("Range Min", &row.range_min, 0.5f, 2.0f, "%.2f")) changed = true;
    if (ImGui::InputFloat("Range Max", &row.range_max, 0.5f, 2.0f, "%.2f")) changed = true;

    ImGui::Separator();
    ImGui::TextUnformatted("Timeline");
    if (ImGui::InputInt("Windup (ms)", &row.windup_ms)) changed = true;
    if (ImGui::InputInt("Impact Delay (ms)", &row.impact_delay_ms)) changed = true;
    if (ImGui::InputInt("Recover (ms)", &row.recover_ms)) changed = true;
    if (ImGui::InputInt("Parry Window (ms)", &row.parry_window_ms)) changed = true;
    if (ImGui::Checkbox("Interruptible", &row.interruptible)) changed = true;

    ImGui::Separator();
    ImGui::TextUnformatted("Damage");
    if (ImGui::InputInt("Base Damage Min", &row.base_damage_min)) changed = true;
    if (ImGui::InputInt("Base Damage Max", &row.base_damage_max)) changed = true;
    if (ImGui::InputFloat("Armor Pierce %", &row.armor_pierce_pct, 1.f, 5.f, "%.2f")) changed = true;

    ImGui::TextUnformatted("Damage Scale JSON");
    ImGui::SameLine();
    if (ImGui::Button("Insert Template##damage_template")) {
        ImGui::OpenPopup("DamageTemplatePicker");
    }
    if (ImGui::BeginPopup("DamageTemplatePicker")) {
        for (const auto& tmpl : kDamageScaleTemplates) {
            if (ImGui::MenuItem(tmpl.name)) {
                row.damage_stat_scale_json = tmpl.json_content;
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tmpl.description);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("Example: {\"scaling\":[{\"stat\":\"STR\",\"coef\":1.5}]}");
    char scale_buf[1024] = {};
    std::strncpy(scale_buf, row.damage_stat_scale_json.c_str(), sizeof(scale_buf) - 1);
    if (ImGui::InputTextMultiline("##damage_scale_json", scale_buf, sizeof(scale_buf), {-1.0f, 58.0f})) {
        row.damage_stat_scale_json = scale_buf;
        changed = true;
    }
    const auto dmg_validation = rco::gue::ValidateDamageStatScaleJSON(row.damage_stat_scale_json);
    if (!dmg_validation.syntactically_valid) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[X] Syntax: %s", dmg_validation.error_message.c_str());
    } else if (!dmg_validation.semantically_valid) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "[!] %s", dmg_validation.error_message.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "[OK] Valid");
    }

    ImGui::TextUnformatted("Crit Policy JSON");
    ImGui::SameLine();
    if (ImGui::Button("Insert Template##crit_template")) {
        ImGui::OpenPopup("CritTemplatePicker");
    }
    if (ImGui::BeginPopup("CritTemplatePicker")) {
        for (const auto& tmpl : kCritPolicyTemplates) {
            if (ImGui::MenuItem(tmpl.name)) {
                row.crit_policy_json = tmpl.json_content;
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tmpl.description);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("Example: {\"base_chance_pct\":5.0,\"scaling_stat\":\"DEX\",\"scaling_softcap_value\":1500,\"scaling_softcap_pct\":0.70,\"damage_multiplier\":1.5}");
    char crit_buf[1024] = {};
    std::strncpy(crit_buf, row.crit_policy_json.c_str(), sizeof(crit_buf) - 1);
    if (ImGui::InputTextMultiline("##crit_policy_json", crit_buf, sizeof(crit_buf), {-1.0f, 58.0f})) {
        row.crit_policy_json = crit_buf;
        changed = true;
    }
    const auto crit_validation = rco::gue::ValidateCritPolicyJSON(row.crit_policy_json);
    if (!crit_validation.syntactically_valid) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[X] Syntax: %s", crit_validation.error_message.c_str());
    } else if (!crit_validation.semantically_valid) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "[!] %s", crit_validation.error_message.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "[OK] Valid");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Telegraph");
    static const char* kTelegraphTypes[] = {"none", "ring_close", "cone", "line"};
    int telegraph_idx = 0;
    for (int i = 0; i < 4; ++i) {
        if (row.telegraph_type == kTelegraphTypes[i]) {
            telegraph_idx = i;
            break;
        }
    }
    if (ImGui::Combo("Telegraph Type", &telegraph_idx, kTelegraphTypes, 4)) {
        row.telegraph_type = kTelegraphTypes[telegraph_idx];
        changed = true;
    }
    if (ImGui::InputFloat("Telegraph Radius", &row.telegraph_radius, 0.25f, 1.0f, "%.2f")) changed = true;

    char color_buf[64] = {};
    std::strncpy(color_buf, row.telegraph_color_rgba.c_str(), sizeof(color_buf) - 1);
    if (ImGui::InputText("Telegraph RGBA", color_buf, sizeof(color_buf))) {
        row.telegraph_color_rgba = color_buf;
        changed = true;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Actions");
    char action_windup_buf[64] = {};
    char action_impact_buf[64] = {};
    char action_recover_buf[64] = {};
    std::strncpy(action_windup_buf, row.action_windup.c_str(), sizeof(action_windup_buf) - 1);
    std::strncpy(action_impact_buf, row.action_impact.c_str(), sizeof(action_impact_buf) - 1);
    std::strncpy(action_recover_buf, row.action_recover.c_str(), sizeof(action_recover_buf) - 1);
    if (ImGui::InputText("Action Windup", action_windup_buf, sizeof(action_windup_buf))) {
        row.action_windup = action_windup_buf;
        changed = true;
    }
    if (ImGui::InputText("Action Impact", action_impact_buf, sizeof(action_impact_buf))) {
        row.action_impact = action_impact_buf;
        changed = true;
    }
    if (ImGui::InputText("Action Recover", action_recover_buf, sizeof(action_recover_buf))) {
        row.action_recover = action_recover_buf;
        changed = true;
    }
    if (ImGui::Checkbox("Allow Action Override", &row.allow_action_override)) changed = true;

    char tags_buf[512] = {};
    std::strncpy(tags_buf, row.allowed_action_tags_json.c_str(), sizeof(tags_buf) - 1);
    if (ImGui::InputText("Allowed Tags JSON", tags_buf, sizeof(tags_buf))) {
        row.allowed_action_tags_json = tags_buf;
        changed = true;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("VFX / SFX");
    if (ImGui::InputInt("VFX Windup", &row.vfx_id_windup)) changed = true;
    if (ImGui::InputInt("VFX Impact", &row.vfx_id_impact)) changed = true;
    if (ImGui::InputInt("SFX Windup", &row.sfx_id_windup)) changed = true;
    if (ImGui::InputInt("SFX Impact", &row.sfx_id_impact)) changed = true;
    ImGui::Separator();
    ImGui::TextUnformatted("FX Asset Paths (Unreal-like, futuro)");
    ImGui::TextDisabled("Example: vfx/cleave_impact.fx or vfx:cleave:impact");
    char vfx_path_windup_buf[256] = {};
    char vfx_path_impact_buf[256] = {};
    char sfx_path_windup_buf[256] = {};
    char sfx_path_impact_buf[256] = {};
    std::strncpy(vfx_path_windup_buf, row.vfx_path_windup.c_str(), sizeof(vfx_path_windup_buf) - 1);
    std::strncpy(vfx_path_impact_buf, row.vfx_path_impact.c_str(), sizeof(vfx_path_impact_buf) - 1);
    std::strncpy(sfx_path_windup_buf, row.sfx_path_windup.c_str(), sizeof(sfx_path_windup_buf) - 1);
    std::strncpy(sfx_path_impact_buf, row.sfx_path_impact.c_str(), sizeof(sfx_path_impact_buf) - 1);
    if (ImGui::InputText("VFX Path Windup", vfx_path_windup_buf, IM_ARRAYSIZE(vfx_path_windup_buf))) {
        row.vfx_path_windup = vfx_path_windup_buf;
        changed = true;
    }
    if (ImGui::InputText("VFX Path Impact", vfx_path_impact_buf, IM_ARRAYSIZE(vfx_path_impact_buf))) {
        row.vfx_path_impact = vfx_path_impact_buf;
        changed = true;
    }
    if (ImGui::InputText("SFX Path Windup", sfx_path_windup_buf, IM_ARRAYSIZE(sfx_path_windup_buf))) {
        row.sfx_path_windup = sfx_path_windup_buf;
        changed = true;
    }
    if (ImGui::InputText("SFX Path Impact", sfx_path_impact_buf, IM_ARRAYSIZE(sfx_path_impact_buf))) {
        row.sfx_path_impact = sfx_path_impact_buf;
        changed = true;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Category & Mastery");

    static const char* kCategories[] = {
        "damage", "heal", "buff", "debuff", "mobility", "utility", "summon"
    };
    int category_idx = 0;
    for (int i = 0; i < static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0])); ++i) {
        if (row.category == kCategories[i]) {
            category_idx = i;
            break;
        }
    }
    if (ImGui::Combo("Category", &category_idx, kCategories,
                     static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0])))) {
        row.category = kCategories[category_idx];
        changed = true;
    }

    if (row.category != "damage") {
        ImGui::TextColored(
            ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
            "Runtime does not yet implement '%s' category. Schema only.",
            row.category.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Mastery Progression");
    if (ImGui::InputInt("XP per Use", &row.mastery_xp_per_use)) changed = true;
    if (ImGui::InputInt("Max Level", &row.mastery_max_level)) changed = true;

    static const char* kCurves[] = {"irregular", "linear", "quadratic", "exponential"};
    int curve_idx = 0;
    for (int i = 0; i < static_cast<int>(sizeof(kCurves) / sizeof(kCurves[0])); ++i) {
        if (row.mastery_xp_curve_type == kCurves[i]) {
            curve_idx = i;
            break;
        }
    }
    if (ImGui::Combo("XP Curve Type", &curve_idx, kCurves,
                     static_cast<int>(sizeof(kCurves) / sizeof(kCurves[0])))) {
        row.mastery_xp_curve_type = kCurves[curve_idx];
        changed = true;
    }

    if (ImGui::InputInt("XP Curve Base", &row.mastery_xp_curve_base)) changed = true;
    if (ImGui::InputFloat("XP Curve Exponent", &row.mastery_xp_curve_exponent, 0.1f, 0.5f, "%.2f")) changed = true;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Used by irregular curves. Higher values make later levels scale faster.");
    }
    if (ImGui::InputFloat("XP Irregularity", &row.mastery_xp_irregularity, 0.05f, 0.2f, "%.2f")) changed = true;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("0.0 = smooth curve, 1.0 = strongest deterministic jitter between levels.");
    }

    if (ImGui::SliderFloat("Primary Bonus per Level (%)",
                           &row.mastery_primary_bonus_per_lvl, 0.0f, 0.5f, "%.3f")) {
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        if (row.category == "damage") {
            ImGui::SetTooltip("Damage multiplier per level above 1.\nE.g., 0.03 = +3%% damage per level.");
        } else if (row.category == "heal") {
            ImGui::SetTooltip("Heal amount multiplier per level above 1.");
        } else {
            ImGui::SetTooltip("Primary bonus (meaning depends on category).");
        }
    }

    if (ImGui::SliderFloat("Cooldown Reduction per Level (%)",
                           &row.mastery_cooldown_redux_per_lvl, 0.0f, 0.1f, "%.3f")) {
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Cooldown reduction per level above 1.\nE.g., 0.01 = -1%% cooldown per level.");
    }

    if (row.mastery_max_level > 1) {
        const float max_bonus = row.mastery_primary_bonus_per_lvl * static_cast<float>(row.mastery_max_level - 1);
        const float max_cd_redux = row.mastery_cooldown_redux_per_lvl * static_cast<float>(row.mastery_max_level - 1);
        ImGui::TextDisabled("At max level (%d): +%.0f%% bonus, -%.0f%% cooldown",
            row.mastery_max_level, max_bonus * 100.0f, max_cd_redux * 100.0f);
    }

    if (ImGui::Checkbox("Enabled", &row.enabled)) changed = true;

    if (row.resource_cost < 0) row.resource_cost = 0;
    if (row.cooldown_ms < 0) row.cooldown_ms = 0;
    if (row.range_min < 0.f) row.range_min = 0.f;
    if (row.range_max < 0.f) row.range_max = 0.f;
    if (row.windup_ms < 0) row.windup_ms = 0;
    if (row.impact_delay_ms < 0) row.impact_delay_ms = 0;
    if (row.recover_ms < 0) row.recover_ms = 0;
    if (row.parry_window_ms < 0) row.parry_window_ms = 0;
    if (row.base_damage_min < 0) row.base_damage_min = 0;
    if (row.base_damage_max < row.base_damage_min) row.base_damage_max = row.base_damage_min;
    if (row.armor_pierce_pct < 0.f) row.armor_pierce_pct = 0.f;
    if (row.telegraph_radius < 0.f) row.telegraph_radius = 0.f;
    if (row.vfx_id_windup < 0) row.vfx_id_windup = 0;
    if (row.vfx_id_impact < 0) row.vfx_id_impact = 0;
    if (row.sfx_id_windup < 0) row.sfx_id_windup = 0;
    if (row.sfx_id_impact < 0) row.sfx_id_impact = 0;
    row.category = ToLowerCopy(TrimCopy(row.category));
    if (!IsAllowedCategory(row.category)) row.category = "damage";
    if (row.mastery_xp_per_use < 1) row.mastery_xp_per_use = 1;
    if (row.mastery_max_level < 1) row.mastery_max_level = 1;
    if (row.mastery_max_level > 100) row.mastery_max_level = 100;
    row.mastery_xp_curve_type = ToLowerCopy(TrimCopy(row.mastery_xp_curve_type));
    if (!IsAllowedCurveType(row.mastery_xp_curve_type)) {
        row.mastery_xp_curve_type = "irregular";
    }
    if (row.mastery_xp_curve_base < 1) row.mastery_xp_curve_base = 1;
    if (row.mastery_xp_curve_exponent <= 0.f) row.mastery_xp_curve_exponent = 2.0f;
    if (row.mastery_xp_irregularity < 0.f) row.mastery_xp_irregularity = 0.f;
    if (row.mastery_xp_irregularity > 1.f) row.mastery_xp_irregularity = 1.f;
    if (row.mastery_primary_bonus_per_lvl < 0.f) row.mastery_primary_bonus_per_lvl = 0.f;
    if (row.mastery_primary_bonus_per_lvl > 0.5f) row.mastery_primary_bonus_per_lvl = 0.5f;
    if (row.mastery_cooldown_redux_per_lvl < 0.f) row.mastery_cooldown_redux_per_lvl = 0.f;
    if (row.mastery_cooldown_redux_per_lvl > 0.1f) row.mastery_cooldown_redux_per_lvl = 0.1f;

    return changed;
}

} // namespace

void CombatAbilitiesTab::SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_msg_, sizeof(status_msg_), fmt, args);
    va_end(args);
}

void CombatAbilitiesTab::EnsureTables(sqlite3* db) {
    if (tables_ensured_) return;

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS ability_templates ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE,"
        "  description TEXT NOT NULL DEFAULT '',"
        "  family TEXT NOT NULL DEFAULT 'melee_special',"
        "  resource_type TEXT NOT NULL DEFAULT 'none',"
        "  resource_cost INTEGER NOT NULL DEFAULT 0,"
        "  cooldown_ms INTEGER NOT NULL DEFAULT 2000,"
        "  range_min REAL NOT NULL DEFAULT 0,"
        "  range_max REAL NOT NULL DEFAULT 2.5,"
        "  windup_ms INTEGER NOT NULL DEFAULT 700,"
        "  impact_delay_ms INTEGER NOT NULL DEFAULT 0,"
        "  recover_ms INTEGER NOT NULL DEFAULT 400,"
        "  parry_window_ms INTEGER NOT NULL DEFAULT 200,"
        "  interruptible INTEGER NOT NULL DEFAULT 1,"
        "  base_damage_min INTEGER NOT NULL DEFAULT 0,"
        "  base_damage_max INTEGER NOT NULL DEFAULT 0,"
        "  damage_stat_scale_json TEXT NOT NULL DEFAULT '',"
        "  armor_pierce_pct REAL NOT NULL DEFAULT 0,"
        "  crit_policy_json TEXT NOT NULL DEFAULT '',"
        "  telegraph_type TEXT NOT NULL DEFAULT 'ring_close',"
        "  telegraph_radius REAL NOT NULL DEFAULT 2.5,"
        "  telegraph_color_rgba TEXT NOT NULL DEFAULT '1,0.2,0.2,0.75',"
        "  action_windup TEXT NOT NULL DEFAULT 'Attack',"
        "  action_impact TEXT NOT NULL DEFAULT 'Attack',"
        "  action_recover TEXT NOT NULL DEFAULT 'Idle',"
        "  allow_action_override INTEGER NOT NULL DEFAULT 0,"
        "  allowed_action_tags_json TEXT NOT NULL DEFAULT '',"
        "  vfx_id_windup INTEGER NOT NULL DEFAULT 0,"
        "  vfx_id_impact INTEGER NOT NULL DEFAULT 0,"
        "  sfx_id_windup INTEGER NOT NULL DEFAULT 0,"
        "  sfx_id_impact INTEGER NOT NULL DEFAULT 0,"
        "  vfx_path_windup TEXT NOT NULL DEFAULT '',"
        "  vfx_path_impact TEXT NOT NULL DEFAULT '',"
        "  sfx_path_windup TEXT NOT NULL DEFAULT '',"
        "  sfx_path_impact TEXT NOT NULL DEFAULT '',"
        "  enabled INTEGER NOT NULL DEFAULT 1"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS npc_ability_loadouts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  npc_spawn_id INTEGER NOT NULL DEFAULT 0,"
        "  actor_def_id INTEGER NOT NULL DEFAULT 0,"
        "  ability_id INTEGER NOT NULL DEFAULT 0,"
        "  priority INTEGER NOT NULL DEFAULT 100,"
        "  weight INTEGER NOT NULL DEFAULT 100,"
        "  min_distance REAL NOT NULL DEFAULT 0,"
        "  max_distance REAL NOT NULL DEFAULT 0,"
        "  min_target_hp_pct REAL NOT NULL DEFAULT 0,"
        "  max_target_hp_pct REAL NOT NULL DEFAULT 100,"
        "  phase_tag TEXT NOT NULL DEFAULT '',"
        "  condition_lua TEXT NOT NULL DEFAULT '',"
        "  enabled INTEGER NOT NULL DEFAULT 1"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_spawn "
        "ON npc_ability_loadouts(npc_spawn_id)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_actor "
        "ON npc_ability_loadouts(actor_def_id)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_ability "
        "ON npc_ability_loadouts(ability_id)",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS npc_combat_profiles ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE,"
        "  global_gcd_ms INTEGER NOT NULL DEFAULT 450,"
        "  decision_tick_ms INTEGER NOT NULL DEFAULT 250,"
        "  aggro_style TEXT NOT NULL DEFAULT 'default',"
        "  allow_chain_cast INTEGER NOT NULL DEFAULT 0,"
        "  max_consecutive_specials INTEGER NOT NULL DEFAULT 1,"
        "  enabled INTEGER NOT NULL DEFAULT 1"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS npc_profile_bindings ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  npc_spawn_id INTEGER NOT NULL DEFAULT 0,"
        "  actor_def_id INTEGER NOT NULL DEFAULT 0,"
        "  profile_id INTEGER NOT NULL DEFAULT 0,"
        "  enabled INTEGER NOT NULL DEFAULT 1"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_spawn "
        "ON npc_profile_bindings(npc_spawn_id)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_actor "
        "ON npc_profile_bindings(actor_def_id)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_profile "
        "ON npc_profile_bindings(profile_id)",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS skill_progression_config ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  xp_per_use INTEGER NOT NULL DEFAULT 10,"
        "  max_level INTEGER NOT NULL DEFAULT 10,"
        "  xp_curve_type TEXT NOT NULL DEFAULT 'irregular',"
        "  xp_curve_base INTEGER NOT NULL DEFAULT 40,"
        "  xp_curve_exponent REAL NOT NULL DEFAULT 2.0,"
        "  xp_irregularity REAL NOT NULL DEFAULT 0.5,"
        "  damage_bonus_per_level REAL NOT NULL DEFAULT 0.03,"
        "  cooldown_redux_per_level REAL NOT NULL DEFAULT 0.01"
        ")",
        nullptr, nullptr, nullptr);

    auto has_column = [&](const char* table, const char* column) -> bool {
        char pragma_sql[256];
        std::snprintf(pragma_sql, sizeof(pragma_sql), "PRAGMA table_info(%s)", table);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, pragma_sql, -1, &stmt, nullptr) != SQLITE_OK) {
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
    };

    auto add_column_if_missing = [&](const char* table, const char* column, const char* column_sql) {
        if (has_column(table, column)) {
            return;
        }
        char alter_sql[512];
        std::snprintf(alter_sql, sizeof(alter_sql), "ALTER TABLE %s ADD COLUMN %s", table, column_sql);
        char* err = nullptr;
        const int rc = sqlite3_exec(db, alter_sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            const bool duplicate = (err != nullptr) && (std::strstr(err, "duplicate column name") != nullptr);
            if (!duplicate) {
                SetStatus("Schema update error (%s): %s", column, err ? err : "unknown error");
            }
        }
        if (err) sqlite3_free(err);
    };

    add_column_if_missing("ability_templates", "description",
        "description TEXT NOT NULL DEFAULT ''");
    add_column_if_missing("ability_templates", "category",
        "category TEXT NOT NULL DEFAULT 'damage'");
    add_column_if_missing("ability_templates", "mastery_xp_per_use",
        "mastery_xp_per_use INTEGER NOT NULL DEFAULT 10");
    add_column_if_missing("ability_templates", "mastery_max_level",
        "mastery_max_level INTEGER NOT NULL DEFAULT 10");
    add_column_if_missing("ability_templates", "mastery_xp_curve_type",
        "mastery_xp_curve_type TEXT NOT NULL DEFAULT 'irregular'");
    add_column_if_missing("ability_templates", "mastery_xp_curve_base",
        "mastery_xp_curve_base INTEGER NOT NULL DEFAULT 40");
    add_column_if_missing("ability_templates", "mastery_xp_curve_exponent",
        "mastery_xp_curve_exponent REAL NOT NULL DEFAULT 2.0");
    add_column_if_missing("ability_templates", "mastery_xp_irregularity",
        "mastery_xp_irregularity REAL NOT NULL DEFAULT 0.5");
    add_column_if_missing("ability_templates", "mastery_primary_bonus_per_lvl",
        "mastery_primary_bonus_per_lvl REAL NOT NULL DEFAULT 0.03");
    add_column_if_missing("ability_templates", "mastery_cooldown_redux_per_lvl",
        "mastery_cooldown_redux_per_lvl REAL NOT NULL DEFAULT 0.01");
    add_column_if_missing("ability_templates", "vfx_path_windup",
        "vfx_path_windup TEXT NOT NULL DEFAULT ''");
    add_column_if_missing("ability_templates", "vfx_path_impact",
        "vfx_path_impact TEXT NOT NULL DEFAULT ''");
    add_column_if_missing("ability_templates", "sfx_path_windup",
        "sfx_path_windup TEXT NOT NULL DEFAULT ''");
    add_column_if_missing("ability_templates", "sfx_path_impact",
        "sfx_path_impact TEXT NOT NULL DEFAULT ''");
    add_column_if_missing("skill_progression_config", "xp_curve_exponent",
        "xp_curve_exponent REAL NOT NULL DEFAULT 2.0");
    add_column_if_missing("skill_progression_config", "xp_irregularity",
        "xp_irregularity REAL NOT NULL DEFAULT 0.5");

    tables_ensured_ = true;
}

void CombatAbilitiesTab::LoadDefaultsIfNeeded(sqlite3* db) {
    if (defaults_loaded_ || !db) return;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT xp_per_use, max_level, xp_curve_type, xp_curve_base, "
        "       xp_curve_exponent, xp_irregularity, "
        "       damage_bonus_per_level, cooldown_redux_per_level "
        "FROM skill_progression_config "
        "ORDER BY id LIMIT 1";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        defaults_loaded_ = true;
        return;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        defaults_.xp_per_use = sqlite3_column_int(stmt, 0);
        defaults_.max_level = sqlite3_column_int(stmt, 1);
        if (const auto* text = sqlite3_column_text(stmt, 2)) {
            defaults_.xp_curve_type = reinterpret_cast<const char*>(text);
        }
        defaults_.xp_curve_base = sqlite3_column_int(stmt, 3);
        defaults_.xp_curve_exponent = static_cast<float>(sqlite3_column_double(stmt, 4));
        defaults_.xp_irregularity = static_cast<float>(sqlite3_column_double(stmt, 5));
        defaults_.damage_bonus_per_level = static_cast<float>(sqlite3_column_double(stmt, 6));
        defaults_.cooldown_redux_per_level = static_cast<float>(sqlite3_column_double(stmt, 7));
    }
    sqlite3_finalize(stmt);

    if (defaults_.xp_per_use < 1) defaults_.xp_per_use = 10;
    if (defaults_.max_level < 1) defaults_.max_level = 10;
    defaults_.xp_curve_type = ToLowerCopy(TrimCopy(defaults_.xp_curve_type));
    if (!IsAllowedCurveType(defaults_.xp_curve_type)) {
        defaults_.xp_curve_type = "irregular";
    }
    if (defaults_.xp_curve_base < 1) defaults_.xp_curve_base = 40;
    if (defaults_.xp_curve_exponent <= 0.f) defaults_.xp_curve_exponent = 2.0f;
    defaults_.xp_irregularity = std::clamp(defaults_.xp_irregularity, 0.0f, 1.0f);
    if (defaults_.damage_bonus_per_level < 0.f) defaults_.damage_bonus_per_level = 0.03f;
    if (defaults_.cooldown_redux_per_level < 0.f) defaults_.cooldown_redux_per_level = 0.01f;

    defaults_loaded_ = true;
}

void CombatAbilitiesTab::FetchNPCSpawns(sqlite3* db) {
    npc_spawns_.clear();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, area_name FROM npc_spawns ORDER BY area_name, name, id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NPCSpawnOption row;
        row.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) row.name = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 2)) row.area_name = reinterpret_cast<const char*>(text);
        char label[256];
        std::snprintf(label, sizeof(label), "[%d] %s (%s)", row.id, row.name.c_str(), row.area_name.c_str());
        row.label = label;
        npc_spawns_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
}

void CombatAbilitiesTab::FetchActorDefs(sqlite3* db) {
    actor_defs_.clear();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name FROM media_actor_defs ORDER BY name, id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ActorDefOption row;
        row.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) row.name = reinterpret_cast<const char*>(text);
        char label[256];
        std::snprintf(label, sizeof(label), "[%d] %s", row.id, row.name.c_str());
        row.label = label;
        actor_defs_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
}

void CombatAbilitiesTab::FetchAbilities(sqlite3* db) {
    abilities_.clear();
    selected_ability_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, name, description, family, category, resource_type, resource_cost, cooldown_ms, "
        "       range_min, range_max, windup_ms, impact_delay_ms, recover_ms, "
        "       parry_window_ms, interruptible, base_damage_min, base_damage_max, "
        "       damage_stat_scale_json, armor_pierce_pct, crit_policy_json, "
        "       telegraph_type, telegraph_radius, telegraph_color_rgba, "
        "       action_windup, action_impact, action_recover, "
        "       allow_action_override, allowed_action_tags_json, "
        "       vfx_id_windup, vfx_id_impact, sfx_id_windup, sfx_id_impact, "
        "       vfx_path_windup, vfx_path_impact, sfx_path_windup, sfx_path_impact, "
        "       mastery_xp_per_use, mastery_max_level, mastery_xp_curve_type, "
        "       mastery_xp_curve_base, mastery_xp_curve_exponent, mastery_xp_irregularity, "
        "       mastery_primary_bonus_per_lvl, mastery_cooldown_redux_per_lvl, "
        "       enabled "
        "FROM ability_templates ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Ability fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CombatAbilityTemplate row;
        row.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) row.name = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 2)) row.description = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 3)) row.family = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 4)) row.category = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 5)) row.resource_type = reinterpret_cast<const char*>(text);
        row.resource_cost = sqlite3_column_int(stmt, 6);
        row.cooldown_ms = sqlite3_column_int(stmt, 7);
        row.range_min = static_cast<float>(sqlite3_column_double(stmt, 8));
        row.range_max = static_cast<float>(sqlite3_column_double(stmt, 9));
        row.windup_ms = sqlite3_column_int(stmt, 10);
        row.impact_delay_ms = sqlite3_column_int(stmt, 11);
        row.recover_ms = sqlite3_column_int(stmt, 12);
        row.parry_window_ms = sqlite3_column_int(stmt, 13);
        row.interruptible = sqlite3_column_int(stmt, 14) != 0;
        row.base_damage_min = sqlite3_column_int(stmt, 15);
        row.base_damage_max = sqlite3_column_int(stmt, 16);
        if (const auto* text = sqlite3_column_text(stmt, 17)) row.damage_stat_scale_json = reinterpret_cast<const char*>(text);
        row.armor_pierce_pct = static_cast<float>(sqlite3_column_double(stmt, 18));
        if (const auto* text = sqlite3_column_text(stmt, 19)) row.crit_policy_json = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 20)) row.telegraph_type = reinterpret_cast<const char*>(text);
        row.telegraph_radius = static_cast<float>(sqlite3_column_double(stmt, 21));
        if (const auto* text = sqlite3_column_text(stmt, 22)) row.telegraph_color_rgba = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 23)) row.action_windup = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 24)) row.action_impact = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 25)) row.action_recover = reinterpret_cast<const char*>(text);
        row.allow_action_override = sqlite3_column_int(stmt, 26) != 0;
        if (const auto* text = sqlite3_column_text(stmt, 27)) row.allowed_action_tags_json = reinterpret_cast<const char*>(text);
        row.vfx_id_windup = sqlite3_column_int(stmt, 28);
        row.vfx_id_impact = sqlite3_column_int(stmt, 29);
        row.sfx_id_windup = sqlite3_column_int(stmt, 30);
        row.sfx_id_impact = sqlite3_column_int(stmt, 31);
        if (const auto* text = sqlite3_column_text(stmt, 32)) row.vfx_path_windup = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 33)) row.vfx_path_impact = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 34)) row.sfx_path_windup = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 35)) row.sfx_path_impact = reinterpret_cast<const char*>(text);
        row.mastery_xp_per_use = sqlite3_column_int(stmt, 36);
        row.mastery_max_level = sqlite3_column_int(stmt, 37);
        if (const auto* text = sqlite3_column_text(stmt, 38)) row.mastery_xp_curve_type = reinterpret_cast<const char*>(text);
        row.mastery_xp_curve_base = sqlite3_column_int(stmt, 39);
        row.mastery_xp_curve_exponent = static_cast<float>(sqlite3_column_double(stmt, 40));
        row.mastery_xp_irregularity = static_cast<float>(sqlite3_column_double(stmt, 41));
        row.mastery_primary_bonus_per_lvl = static_cast<float>(sqlite3_column_double(stmt, 42));
        row.mastery_cooldown_redux_per_lvl = static_cast<float>(sqlite3_column_double(stmt, 43));
        row.enabled = sqlite3_column_int(stmt, 44) != 0;
        abilities_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (!abilities_.empty()) {
        if (select_ability_after_fetch_ > 0) {
            for (int i = 0; i < static_cast<int>(abilities_.size()); ++i) {
                if (abilities_[i].id == select_ability_after_fetch_) {
                    selected_ability_ = i;
                    break;
                }
            }
        }
        if (selected_ability_ < 0) selected_ability_ = 0;
        editing_ability_ = abilities_[selected_ability_];
        dirty_ability_ = false;
    }
    select_ability_after_fetch_ = 0;
}

void CombatAbilitiesTab::FetchLoadouts(sqlite3* db) {
    loadouts_.clear();
    selected_loadout_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, npc_spawn_id, actor_def_id, ability_id, priority, weight, "
        "       min_distance, max_distance, min_target_hp_pct, max_target_hp_pct, "
        "       phase_tag, condition_lua, enabled "
        "FROM npc_ability_loadouts "
        "ORDER BY priority DESC, weight DESC, id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Loadout fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NPCAbilityLoadoutEntry row;
        row.id = sqlite3_column_int(stmt, 0);
        row.npc_spawn_id = sqlite3_column_int(stmt, 1);
        row.actor_def_id = sqlite3_column_int(stmt, 2);
        row.ability_id = sqlite3_column_int(stmt, 3);
        row.priority = sqlite3_column_int(stmt, 4);
        row.weight = sqlite3_column_int(stmt, 5);
        row.min_distance = static_cast<float>(sqlite3_column_double(stmt, 6));
        row.max_distance = static_cast<float>(sqlite3_column_double(stmt, 7));
        row.min_target_hp_pct = static_cast<float>(sqlite3_column_double(stmt, 8));
        row.max_target_hp_pct = static_cast<float>(sqlite3_column_double(stmt, 9));
        if (const auto* text = sqlite3_column_text(stmt, 10)) row.phase_tag = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 11)) row.condition_lua = reinterpret_cast<const char*>(text);
        row.enabled = sqlite3_column_int(stmt, 12) != 0;
        loadouts_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (!loadouts_.empty()) {
        if (select_loadout_after_fetch_ > 0) {
            for (int i = 0; i < static_cast<int>(loadouts_.size()); ++i) {
                if (loadouts_[i].id == select_loadout_after_fetch_) {
                    selected_loadout_ = i;
                    break;
                }
            }
        }
        if (selected_loadout_ < 0) selected_loadout_ = 0;
        editing_loadout_ = loadouts_[selected_loadout_];
        dirty_loadout_ = false;
    }
    select_loadout_after_fetch_ = 0;
}

void CombatAbilitiesTab::FetchProfiles(sqlite3* db) {
    profiles_.clear();
    profile_options_.clear();
    selected_profile_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, name, global_gcd_ms, decision_tick_ms, aggro_style, "
        "       allow_chain_cast, max_consecutive_specials, enabled "
        "FROM npc_combat_profiles ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Profile fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NPCCombatProfile row;
        row.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) row.name = reinterpret_cast<const char*>(text);
        row.global_gcd_ms = sqlite3_column_int(stmt, 2);
        row.decision_tick_ms = sqlite3_column_int(stmt, 3);
        if (const auto* text = sqlite3_column_text(stmt, 4)) row.aggro_style = reinterpret_cast<const char*>(text);
        row.allow_chain_cast = sqlite3_column_int(stmt, 5) != 0;
        row.max_consecutive_specials = sqlite3_column_int(stmt, 6);
        row.enabled = sqlite3_column_int(stmt, 7) != 0;
        profiles_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    for (const auto& profile : profiles_) {
        ProfileOption opt;
        opt.id = profile.id;
        opt.name = profile.name;
        char label[256];
        std::snprintf(label, sizeof(label), "[%d] %s", profile.id, profile.name.c_str());
        opt.label = label;
        profile_options_.push_back(std::move(opt));
    }

    if (!profiles_.empty()) {
        if (select_profile_after_fetch_ > 0) {
            for (int i = 0; i < static_cast<int>(profiles_.size()); ++i) {
                if (profiles_[i].id == select_profile_after_fetch_) {
                    selected_profile_ = i;
                    break;
                }
            }
        }
        if (selected_profile_ < 0) selected_profile_ = 0;
        editing_profile_ = profiles_[selected_profile_];
        dirty_profile_ = false;
    }
    select_profile_after_fetch_ = 0;
}

void CombatAbilitiesTab::FetchProfileBindings(sqlite3* db) {
    profile_bindings_.clear();
    selected_profile_binding_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, npc_spawn_id, actor_def_id, profile_id, enabled "
        "FROM npc_profile_bindings ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Profile binding fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NPCProfileBinding row;
        row.id = sqlite3_column_int(stmt, 0);
        row.npc_spawn_id = sqlite3_column_int(stmt, 1);
        row.actor_def_id = sqlite3_column_int(stmt, 2);
        row.profile_id = sqlite3_column_int(stmt, 3);
        row.enabled = sqlite3_column_int(stmt, 4) != 0;
        profile_bindings_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (!profile_bindings_.empty()) {
        if (select_profile_binding_after_fetch_ > 0) {
            for (int i = 0; i < static_cast<int>(profile_bindings_.size()); ++i) {
                if (profile_bindings_[i].id == select_profile_binding_after_fetch_) {
                    selected_profile_binding_ = i;
                    break;
                }
            }
        }
        if (selected_profile_binding_ < 0) selected_profile_binding_ = 0;
        editing_profile_binding_ = profile_bindings_[selected_profile_binding_];
        dirty_profile_binding_ = false;
    }
    select_profile_binding_after_fetch_ = 0;
}

void CombatAbilitiesTab::FetchAll(sqlite3* db) {
    EnsureTables(db);
    FetchNPCSpawns(db);
    FetchActorDefs(db);
    FetchAbilities(db);
    FetchLoadouts(db);
    FetchProfiles(db);
    FetchProfileBindings(db);
    SetStatus("Loaded %d abilities, %d loadouts, %d profiles and %d profile bindings.",
        static_cast<int>(abilities_.size()),
        static_cast<int>(loadouts_.size()),
        static_cast<int>(profiles_.size()),
        static_cast<int>(profile_bindings_.size()));
}

bool CombatAbilitiesTab::ValidateAbility(sqlite3* db, const CombatAbilityTemplate& row, bool is_new, std::string* out_error) const {
    const std::string name = TrimCopy(row.name);
    if (name.empty()) {
        if (out_error) *out_error = "Ability name is required.";
        return false;
    }
    if (row.range_min < 0.f || row.range_max < 0.f) {
        if (out_error) *out_error = "Range values cannot be negative.";
        return false;
    }
    if (row.range_max < row.range_min) {
        if (out_error) *out_error = "Range max must be >= range min.";
        return false;
    }
    if (row.windup_ms < 0 || row.impact_delay_ms < 0 || row.recover_ms < 0 || row.parry_window_ms < 0) {
        if (out_error) *out_error = "Timeline values cannot be negative.";
        return false;
    }
    if (row.windup_ms < row.parry_window_ms) {
        if (out_error) *out_error = "Windup must be >= parry window.";
        return false;
    }
    if (row.base_damage_min < 0 || row.base_damage_max < 0) {
        if (out_error) *out_error = "Damage values cannot be negative.";
        return false;
    }
    if (row.base_damage_max < row.base_damage_min) {
        if (out_error) *out_error = "Base damage max must be >= base damage min.";
        return false;
    }
    if (row.base_damage_max > 0 && row.cooldown_ms <= 0) {
        if (out_error) *out_error = "Offensive abilities must have cooldown > 0.";
        return false;
    }
    if (row.telegraph_radius < 0.f) {
        if (out_error) *out_error = "Telegraph radius cannot be negative.";
        return false;
    }
    if (row.resource_cost < 0 || row.cooldown_ms < 0) {
        if (out_error) *out_error = "Resource and cooldown cannot be negative.";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    if (is_new) {
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM ability_templates WHERE name=?", -1, &stmt, nullptr) != SQLITE_OK) {
            if (out_error) *out_error = "Failed to validate ability uniqueness.";
            return false;
        }
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM ability_templates WHERE name=? AND id<>?", -1, &stmt, nullptr) != SQLITE_OK) {
            if (out_error) *out_error = "Failed to validate ability uniqueness.";
            return false;
        }
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, row.id);
    }
    int dup_count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        dup_count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (dup_count > 0) {
        if (out_error) *out_error = "Ability name already exists.";
        return false;
    }
    return true;
}

bool CombatAbilitiesTab::ValidateLoadout(sqlite3* db, const NPCAbilityLoadoutEntry& row, std::string* out_error) const {
    if (row.ability_id <= 0) {
        if (out_error) *out_error = "Ability is required.";
        return false;
    }
    if (row.npc_spawn_id <= 0 && row.actor_def_id <= 0) {
        if (out_error) *out_error = "Set NPC Spawn or Actor Def (at least one).";
        return false;
    }
    if (row.priority < 0 || row.weight < 0) {
        if (out_error) *out_error = "Priority and weight must be >= 0.";
        return false;
    }
    if (row.min_distance < 0.f || row.max_distance < 0.f) {
        if (out_error) *out_error = "Distance values cannot be negative.";
        return false;
    }
    if (row.max_distance > 0.f && row.max_distance < row.min_distance) {
        if (out_error) *out_error = "Max distance must be >= min distance.";
        return false;
    }
    if (row.min_target_hp_pct < 0.f || row.min_target_hp_pct > 100.f ||
        row.max_target_hp_pct < 0.f || row.max_target_hp_pct > 100.f) {
        if (out_error) *out_error = "Target HP % values must be within [0, 100].";
        return false;
    }
    if (row.max_target_hp_pct < row.min_target_hp_pct) {
        if (out_error) *out_error = "Max target HP % must be >= min target HP %.";
        return false;
    }

    if (!ExistsID(db, "SELECT COUNT(1) FROM ability_templates WHERE id=?", row.ability_id)) {
        if (out_error) *out_error = "Ability ID does not exist.";
        return false;
    }
    if (row.npc_spawn_id > 0 && !ExistsID(db, "SELECT COUNT(1) FROM npc_spawns WHERE id=?", row.npc_spawn_id)) {
        if (out_error) *out_error = "NPC Spawn ID does not exist.";
        return false;
    }
    if (row.actor_def_id > 0 && !ExistsID(db, "SELECT COUNT(1) FROM media_actor_defs WHERE id=?", row.actor_def_id)) {
        if (out_error) *out_error = "Actor Def ID does not exist.";
        return false;
    }

    return true;
}

bool CombatAbilitiesTab::ValidateProfile(sqlite3* db, const NPCCombatProfile& row, bool is_new, std::string* out_error) const {
    const std::string name = TrimCopy(row.name);
    if (name.empty()) {
        if (out_error) *out_error = "Profile name is required.";
        return false;
    }
    if (row.global_gcd_ms < 0 || row.decision_tick_ms < 0) {
        if (out_error) *out_error = "global_gcd_ms and decision_tick_ms must be >= 0.";
        return false;
    }
    if (row.max_consecutive_specials < 1) {
        if (out_error) *out_error = "max_consecutive_specials must be >= 1.";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    if (is_new) {
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM npc_combat_profiles WHERE name=?", -1, &stmt, nullptr) != SQLITE_OK) {
            if (out_error) *out_error = "Failed to validate profile uniqueness.";
            return false;
        }
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM npc_combat_profiles WHERE name=? AND id<>?", -1, &stmt, nullptr) != SQLITE_OK) {
            if (out_error) *out_error = "Failed to validate profile uniqueness.";
            return false;
        }
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, row.id);
    }
    int dup = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        dup = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (dup > 0) {
        if (out_error) *out_error = "Profile name already exists.";
        return false;
    }
    return true;
}

bool CombatAbilitiesTab::ValidateProfileBinding(sqlite3* db, const NPCProfileBinding& row, std::string* out_error) const {
    if (row.profile_id <= 0) {
        if (out_error) *out_error = "Profile is required.";
        return false;
    }
    if (row.npc_spawn_id <= 0 && row.actor_def_id <= 0) {
        if (out_error) *out_error = "Set NPC Spawn or Actor Def (at least one).";
        return false;
    }
    if (row.npc_spawn_id > 0 && !ExistsID(db, "SELECT COUNT(1) FROM npc_spawns WHERE id=?", row.npc_spawn_id)) {
        if (out_error) *out_error = "NPC Spawn ID does not exist.";
        return false;
    }
    if (row.actor_def_id > 0 && !ExistsID(db, "SELECT COUNT(1) FROM media_actor_defs WHERE id=?", row.actor_def_id)) {
        if (out_error) *out_error = "Actor Def ID does not exist.";
        return false;
    }
    if (!ExistsID(db, "SELECT COUNT(1) FROM npc_combat_profiles WHERE id=?", row.profile_id)) {
        if (out_error) *out_error = "Profile ID does not exist.";
        return false;
    }
    return true;
}

bool CombatAbilitiesTab::SaveAbility(sqlite3* db, CombatAbilityTemplate& row) {
    row.name = TrimCopy(row.name);
    row.description = TrimCopy(row.description);
    row.family = TrimCopy(row.family);
    row.category = ToLowerCopy(TrimCopy(row.category));
    row.resource_type = TrimCopy(row.resource_type);
    row.telegraph_type = TrimCopy(row.telegraph_type);
    row.telegraph_color_rgba = TrimCopy(row.telegraph_color_rgba);
    row.action_windup = TrimCopy(row.action_windup);
    row.action_impact = TrimCopy(row.action_impact);
    row.action_recover = TrimCopy(row.action_recover);
    row.damage_stat_scale_json = TrimCopy(row.damage_stat_scale_json);
    row.crit_policy_json = TrimCopy(row.crit_policy_json);
    row.allowed_action_tags_json = TrimCopy(row.allowed_action_tags_json);
    row.vfx_path_windup = TrimCopy(row.vfx_path_windup);
    row.vfx_path_impact = TrimCopy(row.vfx_path_impact);
    row.sfx_path_windup = TrimCopy(row.sfx_path_windup);
    row.sfx_path_impact = TrimCopy(row.sfx_path_impact);
    row.mastery_xp_curve_type = ToLowerCopy(TrimCopy(row.mastery_xp_curve_type));
    if (row.family.empty()) row.family = "melee_special";
    if (!IsAllowedCategory(row.category)) row.category = "damage";
    if (row.resource_type.empty()) row.resource_type = "none";
    if (row.telegraph_type.empty()) row.telegraph_type = "ring_close";
    if (row.telegraph_color_rgba.empty()) row.telegraph_color_rgba = "1,0.2,0.2,0.75";
    if (row.action_windup.empty()) row.action_windup = "Attack";
    if (row.action_impact.empty()) row.action_impact = "Attack";
    if (row.action_recover.empty()) row.action_recover = "Idle";
    if (row.mastery_xp_per_use < 1) row.mastery_xp_per_use = 1;
    if (row.mastery_max_level < 1) row.mastery_max_level = 1;
    if (row.mastery_max_level > 100) row.mastery_max_level = 100;
    if (!IsAllowedCurveType(row.mastery_xp_curve_type)) {
        row.mastery_xp_curve_type = "irregular";
    }
    if (row.mastery_xp_curve_base < 1) row.mastery_xp_curve_base = 1;
    if (row.mastery_xp_curve_exponent <= 0.f) row.mastery_xp_curve_exponent = 2.0f;
    row.mastery_xp_irregularity = std::clamp(row.mastery_xp_irregularity, 0.0f, 1.0f);
    row.mastery_primary_bonus_per_lvl = std::clamp(row.mastery_primary_bonus_per_lvl, 0.0f, 0.5f);
    row.mastery_cooldown_redux_per_lvl = std::clamp(row.mastery_cooldown_redux_per_lvl, 0.0f, 0.1f);

    const auto dmg_check = rco::gue::ValidateDamageStatScaleJSON(row.damage_stat_scale_json);
    if (!dmg_check.syntactically_valid || !dmg_check.semantically_valid) {
        std::printf(
            "WARN: ability '%s' saved with INVALID damage_scale_json: %s\n",
            row.name.c_str(),
            dmg_check.error_message.c_str());
    }
    const auto crit_check = rco::gue::ValidateCritPolicyJSON(row.crit_policy_json);
    if (!crit_check.syntactically_valid || !crit_check.semantically_valid) {
        std::printf(
            "WARN: ability '%s' saved with INVALID crit_policy_json: %s\n",
            row.name.c_str(),
            crit_check.error_message.c_str());
    }

    std::string validation_error;
    const bool is_new = (row.id == 0);
    if (!ValidateAbility(db, row, is_new, &validation_error)) {
        SetStatus("%s", validation_error.c_str());
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;

    if (is_new) {
        const char* sql =
            "INSERT INTO ability_templates ("
            "name, description, family, category, resource_type, resource_cost, cooldown_ms, "
            "range_min, range_max, windup_ms, impact_delay_ms, recover_ms, "
            "parry_window_ms, interruptible, base_damage_min, base_damage_max, "
            "damage_stat_scale_json, armor_pierce_pct, crit_policy_json, "
            "telegraph_type, telegraph_radius, telegraph_color_rgba, "
            "action_windup, action_impact, action_recover, "
            "allow_action_override, allowed_action_tags_json, "
            "vfx_id_windup, vfx_id_impact, sfx_id_windup, sfx_id_impact, "
            "vfx_path_windup, vfx_path_impact, sfx_path_windup, sfx_path_impact, "
            "mastery_xp_per_use, mastery_max_level, mastery_xp_curve_type, "
            "mastery_xp_curve_base, mastery_xp_curve_exponent, mastery_xp_irregularity, "
            "mastery_primary_bonus_per_lvl, mastery_cooldown_redux_per_lvl, enabled"
            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Ability create error: %s", sqlite3_errmsg(db));
            return false;
        }
    } else {
        const char* sql =
            "UPDATE ability_templates SET "
            "name=?, description=?, family=?, category=?, resource_type=?, resource_cost=?, cooldown_ms=?, "
            "range_min=?, range_max=?, windup_ms=?, impact_delay_ms=?, recover_ms=?, "
            "parry_window_ms=?, interruptible=?, base_damage_min=?, base_damage_max=?, "
            "damage_stat_scale_json=?, armor_pierce_pct=?, crit_policy_json=?, "
            "telegraph_type=?, telegraph_radius=?, telegraph_color_rgba=?, "
            "action_windup=?, action_impact=?, action_recover=?, "
            "allow_action_override=?, allowed_action_tags_json=?, "
            "vfx_id_windup=?, vfx_id_impact=?, sfx_id_windup=?, sfx_id_impact=?, "
            "vfx_path_windup=?, vfx_path_impact=?, sfx_path_windup=?, sfx_path_impact=?, "
            "mastery_xp_per_use=?, mastery_max_level=?, mastery_xp_curve_type=?, "
            "mastery_xp_curve_base=?, mastery_xp_curve_exponent=?, mastery_xp_irregularity=?, "
            "mastery_primary_bonus_per_lvl=?, mastery_cooldown_redux_per_lvl=?, enabled=? "
            "WHERE id=?";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Ability save error: %s", sqlite3_errmsg(db));
            return false;
        }
    }

    sqlite3_bind_text(stmt, 1, row.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, row.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, row.family.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, row.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, row.resource_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, row.resource_cost);
    sqlite3_bind_int(stmt, 7, row.cooldown_ms);
    sqlite3_bind_double(stmt, 8, row.range_min);
    sqlite3_bind_double(stmt, 9, row.range_max);
    sqlite3_bind_int(stmt, 10, row.windup_ms);
    sqlite3_bind_int(stmt, 11, row.impact_delay_ms);
    sqlite3_bind_int(stmt, 12, row.recover_ms);
    sqlite3_bind_int(stmt, 13, row.parry_window_ms);
    sqlite3_bind_int(stmt, 14, row.interruptible ? 1 : 0);
    sqlite3_bind_int(stmt, 15, row.base_damage_min);
    sqlite3_bind_int(stmt, 16, row.base_damage_max);
    sqlite3_bind_text(stmt, 17, row.damage_stat_scale_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 18, row.armor_pierce_pct);
    sqlite3_bind_text(stmt, 19, row.crit_policy_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 20, row.telegraph_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 21, row.telegraph_radius);
    sqlite3_bind_text(stmt, 22, row.telegraph_color_rgba.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 23, row.action_windup.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 24, row.action_impact.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 25, row.action_recover.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 26, row.allow_action_override ? 1 : 0);
    sqlite3_bind_text(stmt, 27, row.allowed_action_tags_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 28, row.vfx_id_windup);
    sqlite3_bind_int(stmt, 29, row.vfx_id_impact);
    sqlite3_bind_int(stmt, 30, row.sfx_id_windup);
    sqlite3_bind_int(stmt, 31, row.sfx_id_impact);
    sqlite3_bind_text(stmt, 32, row.vfx_path_windup.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 33, row.vfx_path_impact.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 34, row.sfx_path_windup.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 35, row.sfx_path_impact.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 36, row.mastery_xp_per_use);
    sqlite3_bind_int(stmt, 37, row.mastery_max_level);
    sqlite3_bind_text(stmt, 38, row.mastery_xp_curve_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 39, row.mastery_xp_curve_base);
    sqlite3_bind_double(stmt, 40, row.mastery_xp_curve_exponent);
    sqlite3_bind_double(stmt, 41, row.mastery_xp_irregularity);
    sqlite3_bind_double(stmt, 42, row.mastery_primary_bonus_per_lvl);
    sqlite3_bind_double(stmt, 43, row.mastery_cooldown_redux_per_lvl);
    sqlite3_bind_int(stmt, 44, row.enabled ? 1 : 0);
    if (!is_new) {
        sqlite3_bind_int(stmt, 45, row.id);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        SetStatus("Ability save error: %s", sqlite3_errmsg(db));
        return false;
    }

    if (is_new) {
        row.id = static_cast<int>(sqlite3_last_insert_rowid(db));
    }
    select_ability_after_fetch_ = row.id;
    need_fetch_ = true;
    dirty_ability_ = false;
    show_new_ability_ = false;
    SetStatus("Saved ability '%s' (id=%d).", row.name.c_str(), row.id);
    return true;
}

bool CombatAbilitiesTab::DeleteAbility(sqlite3* db, int ability_id) {
    sqlite3_stmt* guard = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM npc_ability_loadouts WHERE ability_id=?", -1, &guard, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(guard, 1, ability_id);
        int refs = 0;
        if (sqlite3_step(guard) == SQLITE_ROW) refs = sqlite3_column_int(guard, 0);
        sqlite3_finalize(guard);
        if (refs > 0) {
            SetStatus("Cannot delete ability %d: used by %d loadout row(s).", ability_id, refs);
            return false;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM ability_templates WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Ability delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, ability_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Ability delete error: %s", sqlite3_errmsg(db));
        return false;
    }

    need_fetch_ = true;
    selected_ability_ = -1;
    SetStatus("Deleted ability %d.", ability_id);
    return true;
}

bool CombatAbilitiesTab::SaveLoadout(sqlite3* db, NPCAbilityLoadoutEntry& row) {
    row.phase_tag = TrimCopy(row.phase_tag);
    row.condition_lua = TrimCopy(row.condition_lua);

    std::string validation_error;
    if (!ValidateLoadout(db, row, &validation_error)) {
        SetStatus("%s", validation_error.c_str());
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    if (row.id == 0) {
        const char* sql =
            "INSERT INTO npc_ability_loadouts ("
            "npc_spawn_id, actor_def_id, ability_id, priority, weight, "
            "min_distance, max_distance, min_target_hp_pct, max_target_hp_pct, "
            "phase_tag, condition_lua, enabled"
            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Loadout create error: %s", sqlite3_errmsg(db));
            return false;
        }
    } else {
        const char* sql =
            "UPDATE npc_ability_loadouts SET "
            "npc_spawn_id=?, actor_def_id=?, ability_id=?, priority=?, weight=?, "
            "min_distance=?, max_distance=?, min_target_hp_pct=?, max_target_hp_pct=?, "
            "phase_tag=?, condition_lua=?, enabled=? "
            "WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Loadout save error: %s", sqlite3_errmsg(db));
            return false;
        }
    }

    sqlite3_bind_int(stmt, 1, row.npc_spawn_id);
    sqlite3_bind_int(stmt, 2, row.actor_def_id);
    sqlite3_bind_int(stmt, 3, row.ability_id);
    sqlite3_bind_int(stmt, 4, row.priority);
    sqlite3_bind_int(stmt, 5, row.weight);
    sqlite3_bind_double(stmt, 6, row.min_distance);
    sqlite3_bind_double(stmt, 7, row.max_distance);
    sqlite3_bind_double(stmt, 8, row.min_target_hp_pct);
    sqlite3_bind_double(stmt, 9, row.max_target_hp_pct);
    sqlite3_bind_text(stmt, 10, row.phase_tag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, row.condition_lua.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 12, row.enabled ? 1 : 0);
    if (row.id > 0) {
        sqlite3_bind_int(stmt, 13, row.id);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Loadout save error: %s", sqlite3_errmsg(db));
        return false;
    }

    if (row.id == 0) {
        row.id = static_cast<int>(sqlite3_last_insert_rowid(db));
    }
    select_loadout_after_fetch_ = row.id;
    need_fetch_ = true;
    dirty_loadout_ = false;
    show_new_loadout_ = false;
    SetStatus("Saved loadout id=%d.", row.id);
    return true;
}

bool CombatAbilitiesTab::DeleteLoadout(sqlite3* db, int loadout_id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM npc_ability_loadouts WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Loadout delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, loadout_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Loadout delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    need_fetch_ = true;
    selected_loadout_ = -1;
    SetStatus("Deleted loadout %d.", loadout_id);
    return true;
}

bool CombatAbilitiesTab::SaveProfile(sqlite3* db, NPCCombatProfile& row) {
    row.name = TrimCopy(row.name);
    row.aggro_style = TrimCopy(row.aggro_style);
    if (row.aggro_style.empty()) row.aggro_style = "default";
    if (row.global_gcd_ms < 0) row.global_gcd_ms = 0;
    if (row.decision_tick_ms < 0) row.decision_tick_ms = 0;
    if (row.max_consecutive_specials < 1) row.max_consecutive_specials = 1;

    std::string validation_error;
    const bool is_new = (row.id == 0);
    if (!ValidateProfile(db, row, is_new, &validation_error)) {
        SetStatus("%s", validation_error.c_str());
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    if (is_new) {
        const char* sql =
            "INSERT INTO npc_combat_profiles ("
            "name, global_gcd_ms, decision_tick_ms, aggro_style, "
            "allow_chain_cast, max_consecutive_specials, enabled"
            ") VALUES (?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Profile create error: %s", sqlite3_errmsg(db));
            return false;
        }
    } else {
        const char* sql =
            "UPDATE npc_combat_profiles SET "
            "name=?, global_gcd_ms=?, decision_tick_ms=?, aggro_style=?, "
            "allow_chain_cast=?, max_consecutive_specials=?, enabled=? "
            "WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Profile save error: %s", sqlite3_errmsg(db));
            return false;
        }
    }

    sqlite3_bind_text(stmt, 1, row.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, row.global_gcd_ms);
    sqlite3_bind_int(stmt, 3, row.decision_tick_ms);
    sqlite3_bind_text(stmt, 4, row.aggro_style.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, row.allow_chain_cast ? 1 : 0);
    sqlite3_bind_int(stmt, 6, row.max_consecutive_specials);
    sqlite3_bind_int(stmt, 7, row.enabled ? 1 : 0);
    if (!is_new) {
        sqlite3_bind_int(stmt, 8, row.id);
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Profile save error: %s", sqlite3_errmsg(db));
        return false;
    }

    if (is_new) {
        row.id = static_cast<int>(sqlite3_last_insert_rowid(db));
    }
    select_profile_after_fetch_ = row.id;
    need_fetch_ = true;
    dirty_profile_ = false;
    show_new_profile_ = false;
    SetStatus("Saved profile '%s' (id=%d).", row.name.c_str(), row.id);
    return true;
}

bool CombatAbilitiesTab::DeleteProfile(sqlite3* db, int profile_id) {
    sqlite3_stmt* guard = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM npc_profile_bindings WHERE profile_id=?", -1, &guard, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(guard, 1, profile_id);
        int refs = 0;
        if (sqlite3_step(guard) == SQLITE_ROW) refs = sqlite3_column_int(guard, 0);
        sqlite3_finalize(guard);
        if (refs > 0) {
            SetStatus("Cannot delete profile %d: used by %d profile binding(s).", profile_id, refs);
            return false;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM npc_combat_profiles WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Profile delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, profile_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Profile delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    need_fetch_ = true;
    selected_profile_ = -1;
    SetStatus("Deleted profile %d.", profile_id);
    return true;
}

bool CombatAbilitiesTab::SaveProfileBinding(sqlite3* db, NPCProfileBinding& row) {
    std::string validation_error;
    if (!ValidateProfileBinding(db, row, &validation_error)) {
        SetStatus("%s", validation_error.c_str());
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    if (row.id == 0) {
        const char* sql =
            "INSERT INTO npc_profile_bindings ("
            "npc_spawn_id, actor_def_id, profile_id, enabled"
            ") VALUES (?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Profile binding create error: %s", sqlite3_errmsg(db));
            return false;
        }
    } else {
        const char* sql =
            "UPDATE npc_profile_bindings SET "
            "npc_spawn_id=?, actor_def_id=?, profile_id=?, enabled=? "
            "WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Profile binding save error: %s", sqlite3_errmsg(db));
            return false;
        }
    }

    sqlite3_bind_int(stmt, 1, row.npc_spawn_id);
    sqlite3_bind_int(stmt, 2, row.actor_def_id);
    sqlite3_bind_int(stmt, 3, row.profile_id);
    sqlite3_bind_int(stmt, 4, row.enabled ? 1 : 0);
    if (row.id > 0) {
        sqlite3_bind_int(stmt, 5, row.id);
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Profile binding save error: %s", sqlite3_errmsg(db));
        return false;
    }
    if (row.id == 0) {
        row.id = static_cast<int>(sqlite3_last_insert_rowid(db));
    }
    select_profile_binding_after_fetch_ = row.id;
    need_fetch_ = true;
    dirty_profile_binding_ = false;
    show_new_profile_binding_ = false;
    SetStatus("Saved profile binding id=%d.", row.id);
    return true;
}

bool CombatAbilitiesTab::DeleteProfileBinding(sqlite3* db, int binding_id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM npc_profile_bindings WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Profile binding delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, binding_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Profile binding delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    need_fetch_ = true;
    selected_profile_binding_ = -1;
    SetStatus("Deleted profile binding %d.", binding_id);
    return true;
}

const char* CombatAbilitiesTab::AbilityNameByID(int ability_id) const {
    if (ability_id <= 0) return "None (0)";
    for (const auto& row : abilities_) {
        if (row.id == ability_id) return row.name.c_str();
    }
    return "(missing ability)";
}

const char* CombatAbilitiesTab::SpawnLabelByID(int spawn_id) const {
    if (spawn_id <= 0) return "None (0)";
    for (const auto& row : npc_spawns_) {
        if (row.id == spawn_id) return row.label.c_str();
    }
    return "(missing spawn)";
}

const char* CombatAbilitiesTab::ActorDefLabelByID(int actor_def_id) const {
    if (actor_def_id <= 0) return "None (0)";
    for (const auto& row : actor_defs_) {
        if (row.id == actor_def_id) return row.label.c_str();
    }
    return "(missing actor def)";
}

const char* CombatAbilitiesTab::ProfileNameByID(int profile_id) const {
    if (profile_id <= 0) return "None (0)";
    for (const auto& row : profile_options_) {
        if (row.id == profile_id) return row.name.c_str();
    }
    return "(missing profile)";
}

void CombatAbilitiesTab::Draw(sqlite3* db) {
    if (!db) return;
    EnsureTables(db);
    LoadDefaultsIfNeeded(db);
    if (need_fetch_) {
        FetchAll(db);
        need_fetch_ = false;
    }

    if (ImGui::Button("Refresh")) {
        need_fetch_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();
    ImGui::TextWrapped(
        "Define each skill's mastery: how XP scales, what bonus it grants per level, "
        "and what category it belongs to. Defaults come from skill_progression_config "
        "(global template) but each skill is autonomous.");
    ImGui::TextWrapped(
        "Note: only 'damage' category has runtime implementation. Other categories "
        "are schema-only - assigning them won't make skills work yet.");
    ImGui::Separator();

    if (!ImGui::BeginTabBar("##combat_ability_tabs")) {
        return;
    }

    if (ImGui::BeginTabItem("Ability Templates")) {
        if (ImGui::Button("New Ability")) {
            show_new_ability_ = true;
            show_new_loadout_ = false;
            new_ability_ = {};
            new_ability_.category = "damage";
            new_ability_.mastery_xp_per_use = defaults_.xp_per_use;
            new_ability_.mastery_max_level = defaults_.max_level;
            new_ability_.mastery_xp_curve_type = defaults_.xp_curve_type;
            new_ability_.mastery_xp_curve_base = defaults_.xp_curve_base;
            new_ability_.mastery_xp_curve_exponent = defaults_.xp_curve_exponent;
            new_ability_.mastery_xp_irregularity = defaults_.xp_irregularity;
            new_ability_.mastery_primary_bonus_per_lvl = defaults_.damage_bonus_per_level;
            new_ability_.mastery_cooldown_redux_per_lvl = defaults_.cooldown_redux_per_level;
            selected_ability_ = -1;
            dirty_ability_ = false;
        }
        ImGui::Separator();

        const float list_width = 360.f;
        ImGui::BeginChild("##ability_list", {list_width, 0.0f}, true);
        for (int i = 0; i < static_cast<int>(abilities_.size()); ++i) {
            const auto& row = abilities_[i];
            char label[320];
            std::snprintf(label, sizeof(label), "[%d] %s (%s)%s##ability_%d",
                          row.id, row.name.c_str(), row.family.c_str(),
                          row.enabled ? "" : " [disabled]", row.id);
            if (ImGui::Selectable(label, selected_ability_ == i)) {
                selected_ability_ = i;
                editing_ability_ = row;
                dirty_ability_ = false;
                show_new_ability_ = false;
            }
        }
        if (abilities_.empty()) {
            ImGui::TextDisabled("No ability templates.");
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##ability_editor", {0.0f, 0.0f}, true);
        if (show_new_ability_) {
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Ability");
            ImGui::Separator();
            DrawAbilityFields(new_ability_);
            ImGui::Spacing();
            if (ImGui::Button("Create Ability")) {
                SaveAbility(db, new_ability_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##new_ability")) {
                show_new_ability_ = false;
            }
        } else if (selected_ability_ >= 0 && selected_ability_ < static_cast<int>(abilities_.size())) {
            ImGui::Text("Editing Ability [id=%d]", editing_ability_.id);
            ImGui::Separator();
            if (DrawAbilityFields(editing_ability_)) {
                dirty_ability_ = true;
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirty_ability_);
            if (ImGui::Button("Save Ability")) {
                SaveAbility(db, editing_ability_);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Ability")) {
                editing_ability_ = abilities_[selected_ability_];
                dirty_ability_ = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.0f});
            if (ImGui::Button("Delete Ability")) {
                DeleteAbility(db, editing_ability_.id);
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Select an ability, or click \"New Ability\".");
        }
        ImGui::EndChild();

        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("NPC Loadouts")) {
        if (ImGui::Button("New Loadout")) {
            show_new_loadout_ = true;
            show_new_ability_ = false;
            new_loadout_ = {};
            selected_loadout_ = -1;
            dirty_loadout_ = false;
        }
        ImGui::Separator();

        const float list_width = 390.f;
        ImGui::BeginChild("##loadout_list", {list_width, 0.0f}, true);
        for (int i = 0; i < static_cast<int>(loadouts_.size()); ++i) {
            const auto& row = loadouts_[i];
            char label[512];
            std::snprintf(label, sizeof(label),
                "[%d] ab:%s | spawn:%s | def:%s | prio:%d wt:%d%s##loadout_%d",
                row.id,
                AbilityNameByID(row.ability_id),
                SpawnLabelByID(row.npc_spawn_id),
                ActorDefLabelByID(row.actor_def_id),
                row.priority,
                row.weight,
                row.enabled ? "" : " [disabled]",
                row.id);
            if (ImGui::Selectable(label, selected_loadout_ == i)) {
                selected_loadout_ = i;
                editing_loadout_ = row;
                dirty_loadout_ = false;
                show_new_loadout_ = false;
            }
        }
        if (loadouts_.empty()) {
            ImGui::TextDisabled("No NPC ability loadouts.");
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##loadout_editor", {0.0f, 0.0f}, true);

        auto draw_loadout_fields = [&](NPCAbilityLoadoutEntry& row) -> bool {
            bool changed = false;

            std::string ability_preview = "None (0)";
            if (row.ability_id > 0) {
                char tmp[256];
                std::snprintf(tmp, sizeof(tmp), "[%d] %s", row.ability_id, AbilityNameByID(row.ability_id));
                ability_preview = tmp;
            }
            if (ImGui::BeginCombo("Ability", ability_preview.c_str())) {
                bool selected_none = (row.ability_id == 0);
                if (ImGui::Selectable("None (0)", selected_none)) {
                    row.ability_id = 0;
                    changed = true;
                }
                if (selected_none) ImGui::SetItemDefaultFocus();
                for (const auto& ability : abilities_) {
                    char label[256];
                    std::snprintf(label, sizeof(label), "[%d] %s", ability.id, ability.name.c_str());
                    const bool selected = (row.ability_id == ability.id);
                    if (ImGui::Selectable(label, selected)) {
                        row.ability_id = ability.id;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::InputInt("Ability ID", &row.ability_id)) {
                if (row.ability_id < 0) row.ability_id = 0;
                changed = true;
            }

            std::string spawn_preview = SpawnLabelByID(row.npc_spawn_id);
            if (ImGui::BeginCombo("NPC Spawn", spawn_preview.c_str())) {
                bool selected_none = (row.npc_spawn_id == 0);
                if (ImGui::Selectable("None (0)", selected_none)) {
                    row.npc_spawn_id = 0;
                    changed = true;
                }
                if (selected_none) ImGui::SetItemDefaultFocus();
                for (const auto& spawn : npc_spawns_) {
                    bool selected = (row.npc_spawn_id == spawn.id);
                    if (ImGui::Selectable(spawn.label.c_str(), selected)) {
                        row.npc_spawn_id = spawn.id;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::InputInt("NPC Spawn ID", &row.npc_spawn_id)) {
                if (row.npc_spawn_id < 0) row.npc_spawn_id = 0;
                changed = true;
            }

            std::string actor_preview = ActorDefLabelByID(row.actor_def_id);
            if (ImGui::BeginCombo("Actor Def", actor_preview.c_str())) {
                bool selected_none = (row.actor_def_id == 0);
                if (ImGui::Selectable("None (0)", selected_none)) {
                    row.actor_def_id = 0;
                    changed = true;
                }
                if (selected_none) ImGui::SetItemDefaultFocus();
                for (const auto& actor : actor_defs_) {
                    bool selected = (row.actor_def_id == actor.id);
                    if (ImGui::Selectable(actor.label.c_str(), selected)) {
                        row.actor_def_id = actor.id;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::InputInt("Actor Def ID", &row.actor_def_id)) {
                if (row.actor_def_id < 0) row.actor_def_id = 0;
                changed = true;
            }

            if (ImGui::InputInt("Priority", &row.priority)) changed = true;
            if (ImGui::InputInt("Weight", &row.weight)) changed = true;
            if (ImGui::InputFloat("Min Distance", &row.min_distance, 0.5f, 2.0f, "%.2f")) changed = true;
            if (ImGui::InputFloat("Max Distance", &row.max_distance, 0.5f, 2.0f, "%.2f")) changed = true;
            if (ImGui::InputFloat("Min Target HP %", &row.min_target_hp_pct, 1.f, 5.f, "%.1f")) changed = true;
            if (ImGui::InputFloat("Max Target HP %", &row.max_target_hp_pct, 1.f, 5.f, "%.1f")) changed = true;

            char phase_buf[64] = {};
            std::strncpy(phase_buf, row.phase_tag.c_str(), sizeof(phase_buf) - 1);
            if (ImGui::InputText("Phase Tag", phase_buf, sizeof(phase_buf))) {
                row.phase_tag = phase_buf;
                changed = true;
            }

            char cond_buf[1536] = {};
            std::strncpy(cond_buf, row.condition_lua.c_str(), sizeof(cond_buf) - 1);
            ImGui::TextUnformatted("Condition Lua");
            if (ImGui::InputTextMultiline("##loadout_condition_lua", cond_buf, sizeof(cond_buf), {-1.0f, 72.0f})) {
                row.condition_lua = cond_buf;
                changed = true;
            }

            if (ImGui::Checkbox("Enabled", &row.enabled)) changed = true;

            if (row.priority < 0) row.priority = 0;
            if (row.weight < 0) row.weight = 0;
            if (row.min_distance < 0.f) row.min_distance = 0.f;
            if (row.max_distance < 0.f) row.max_distance = 0.f;
            row.min_target_hp_pct = std::clamp(row.min_target_hp_pct, 0.0f, 100.0f);
            row.max_target_hp_pct = std::clamp(row.max_target_hp_pct, 0.0f, 100.0f);

            if (row.npc_spawn_id <= 0 && row.actor_def_id <= 0) {
                ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f}, "Set NPC Spawn or Actor Def.");
            }

            return changed;
        };

        if (show_new_loadout_) {
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Loadout");
            ImGui::Separator();
            draw_loadout_fields(new_loadout_);
            ImGui::Spacing();
            if (ImGui::Button("Create Loadout")) {
                SaveLoadout(db, new_loadout_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##new_loadout")) {
                show_new_loadout_ = false;
            }
        } else if (selected_loadout_ >= 0 && selected_loadout_ < static_cast<int>(loadouts_.size())) {
            ImGui::Text("Editing Loadout [id=%d]", editing_loadout_.id);
            ImGui::Separator();
            if (draw_loadout_fields(editing_loadout_)) {
                dirty_loadout_ = true;
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirty_loadout_);
            if (ImGui::Button("Save Loadout")) {
                SaveLoadout(db, editing_loadout_);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Loadout")) {
                editing_loadout_ = loadouts_[selected_loadout_];
                dirty_loadout_ = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.0f});
            if (ImGui::Button("Delete Loadout")) {
                DeleteLoadout(db, editing_loadout_.id);
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Select a loadout, or click \"New Loadout\".");
        }
        ImGui::EndChild();

        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Combat Profiles")) {
        if (ImGui::Button("New Profile")) {
            show_new_profile_ = true;
            new_profile_ = {};
            selected_profile_ = -1;
            dirty_profile_ = false;
        }
        ImGui::Separator();

        const float list_width = 340.f;
        ImGui::BeginChild("##profile_list", {list_width, 0.0f}, true);
        for (int i = 0; i < static_cast<int>(profiles_.size()); ++i) {
            const auto& row = profiles_[i];
            char label[320];
            std::snprintf(label, sizeof(label), "[%d] %s%s##profile_%d",
                row.id, row.name.c_str(), row.enabled ? "" : " [disabled]", row.id);
            if (ImGui::Selectable(label, selected_profile_ == i)) {
                selected_profile_ = i;
                editing_profile_ = row;
                dirty_profile_ = false;
                show_new_profile_ = false;
            }
        }
        if (profiles_.empty()) {
            ImGui::TextDisabled("No combat profiles.");
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##profile_editor", {0.0f, 0.0f}, true);

        auto draw_profile_fields = [&](NPCCombatProfile& row) -> bool {
            bool changed = false;
            char name_buf[128] = {};
            char style_buf[64] = {};
            std::strncpy(name_buf, row.name.c_str(), sizeof(name_buf) - 1);
            std::strncpy(style_buf, row.aggro_style.c_str(), sizeof(style_buf) - 1);
            if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
                row.name = name_buf;
                changed = true;
            }
            if (ImGui::InputInt("Global GCD (ms)", &row.global_gcd_ms)) changed = true;
            if (ImGui::InputInt("Decision Tick (ms)", &row.decision_tick_ms)) changed = true;
            if (ImGui::InputText("Aggro Style", style_buf, sizeof(style_buf))) {
                row.aggro_style = style_buf;
                changed = true;
            }
            if (ImGui::Checkbox("Allow Chain Cast", &row.allow_chain_cast)) changed = true;
            if (ImGui::InputInt("Max Consecutive Specials", &row.max_consecutive_specials)) changed = true;
            if (ImGui::Checkbox("Enabled", &row.enabled)) changed = true;

            if (row.global_gcd_ms < 0) row.global_gcd_ms = 0;
            if (row.decision_tick_ms < 0) row.decision_tick_ms = 0;
            if (row.max_consecutive_specials < 1) row.max_consecutive_specials = 1;
            if (TrimCopy(row.name).empty()) {
                ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f}, "Profile name is required.");
            }
            return changed;
        };

        if (show_new_profile_) {
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Profile");
            ImGui::Separator();
            draw_profile_fields(new_profile_);
            ImGui::Spacing();
            if (ImGui::Button("Create Profile")) {
                SaveProfile(db, new_profile_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##new_profile")) {
                show_new_profile_ = false;
            }
        } else if (selected_profile_ >= 0 && selected_profile_ < static_cast<int>(profiles_.size())) {
            ImGui::Text("Editing Profile [id=%d]", editing_profile_.id);
            ImGui::Separator();
            if (draw_profile_fields(editing_profile_)) {
                dirty_profile_ = true;
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirty_profile_);
            if (ImGui::Button("Save Profile")) {
                SaveProfile(db, editing_profile_);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Profile")) {
                editing_profile_ = profiles_[selected_profile_];
                dirty_profile_ = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.0f});
            if (ImGui::Button("Delete Profile")) {
                DeleteProfile(db, editing_profile_.id);
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Select a profile, or click \"New Profile\".");
        }
        ImGui::EndChild();

        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Profile Bindings")) {
        if (ImGui::Button("New Binding")) {
            show_new_profile_binding_ = true;
            new_profile_binding_ = {};
            selected_profile_binding_ = -1;
            dirty_profile_binding_ = false;
        }
        ImGui::Separator();

        const float list_width = 440.f;
        ImGui::BeginChild("##profile_binding_list", {list_width, 0.0f}, true);
        for (int i = 0; i < static_cast<int>(profile_bindings_.size()); ++i) {
            const auto& row = profile_bindings_[i];
            char label[512];
            std::snprintf(label, sizeof(label),
                "[%d] profile:%s | spawn:%s | def:%s%s##profile_binding_%d",
                row.id,
                ProfileNameByID(row.profile_id),
                SpawnLabelByID(row.npc_spawn_id),
                ActorDefLabelByID(row.actor_def_id),
                row.enabled ? "" : " [disabled]",
                row.id);
            if (ImGui::Selectable(label, selected_profile_binding_ == i)) {
                selected_profile_binding_ = i;
                editing_profile_binding_ = row;
                dirty_profile_binding_ = false;
                show_new_profile_binding_ = false;
            }
        }
        if (profile_bindings_.empty()) {
            ImGui::TextDisabled("No profile bindings.");
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##profile_binding_editor", {0.0f, 0.0f}, true);

        auto draw_profile_binding_fields = [&](NPCProfileBinding& row) -> bool {
            bool changed = false;

            std::string profile_preview = "None (0)";
            if (row.profile_id > 0) {
                char tmp[256];
                std::snprintf(tmp, sizeof(tmp), "[%d] %s", row.profile_id, ProfileNameByID(row.profile_id));
                profile_preview = tmp;
            }
            if (ImGui::BeginCombo("Profile", profile_preview.c_str())) {
                bool selected_none = (row.profile_id == 0);
                if (ImGui::Selectable("None (0)", selected_none)) {
                    row.profile_id = 0;
                    changed = true;
                }
                if (selected_none) ImGui::SetItemDefaultFocus();
                for (const auto& profile : profile_options_) {
                    bool selected = (row.profile_id == profile.id);
                    if (ImGui::Selectable(profile.label.c_str(), selected)) {
                        row.profile_id = profile.id;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::InputInt("Profile ID", &row.profile_id)) {
                if (row.profile_id < 0) row.profile_id = 0;
                changed = true;
            }

            std::string spawn_preview = SpawnLabelByID(row.npc_spawn_id);
            if (ImGui::BeginCombo("NPC Spawn", spawn_preview.c_str())) {
                bool selected_none = (row.npc_spawn_id == 0);
                if (ImGui::Selectable("None (0)", selected_none)) {
                    row.npc_spawn_id = 0;
                    changed = true;
                }
                if (selected_none) ImGui::SetItemDefaultFocus();
                for (const auto& spawn : npc_spawns_) {
                    bool selected = (row.npc_spawn_id == spawn.id);
                    if (ImGui::Selectable(spawn.label.c_str(), selected)) {
                        row.npc_spawn_id = spawn.id;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::InputInt("NPC Spawn ID", &row.npc_spawn_id)) {
                if (row.npc_spawn_id < 0) row.npc_spawn_id = 0;
                changed = true;
            }

            std::string actor_preview = ActorDefLabelByID(row.actor_def_id);
            if (ImGui::BeginCombo("Actor Def", actor_preview.c_str())) {
                bool selected_none = (row.actor_def_id == 0);
                if (ImGui::Selectable("None (0)", selected_none)) {
                    row.actor_def_id = 0;
                    changed = true;
                }
                if (selected_none) ImGui::SetItemDefaultFocus();
                for (const auto& actor : actor_defs_) {
                    bool selected = (row.actor_def_id == actor.id);
                    if (ImGui::Selectable(actor.label.c_str(), selected)) {
                        row.actor_def_id = actor.id;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::InputInt("Actor Def ID", &row.actor_def_id)) {
                if (row.actor_def_id < 0) row.actor_def_id = 0;
                changed = true;
            }

            if (ImGui::Checkbox("Enabled", &row.enabled)) changed = true;

            if (row.npc_spawn_id <= 0 && row.actor_def_id <= 0) {
                ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f}, "Set NPC Spawn or Actor Def.");
            }
            return changed;
        };

        if (show_new_profile_binding_) {
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Profile Binding");
            ImGui::Separator();
            draw_profile_binding_fields(new_profile_binding_);
            ImGui::Spacing();
            if (ImGui::Button("Create Binding")) {
                SaveProfileBinding(db, new_profile_binding_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##new_profile_binding")) {
                show_new_profile_binding_ = false;
            }
        } else if (selected_profile_binding_ >= 0 && selected_profile_binding_ < static_cast<int>(profile_bindings_.size())) {
            ImGui::Text("Editing Profile Binding [id=%d]", editing_profile_binding_.id);
            ImGui::Separator();
            if (draw_profile_binding_fields(editing_profile_binding_)) {
                dirty_profile_binding_ = true;
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirty_profile_binding_);
            if (ImGui::Button("Save Binding")) {
                SaveProfileBinding(db, editing_profile_binding_);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Binding")) {
                editing_profile_binding_ = profile_bindings_[selected_profile_binding_];
                dirty_profile_binding_ = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.0f});
            if (ImGui::Button("Delete Binding")) {
                DeleteProfileBinding(db, editing_profile_binding_.id);
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Select a profile binding, or click \"New Binding\".");
        }

        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

} // namespace gue
