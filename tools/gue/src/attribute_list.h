#pragma once

#include <string>
#include <utility>
#include <vector>

namespace gue {

// NOTE: 3rd copy of attribute registry on editor side.
// Server: server/internal/world/attributes.go
// Client: client/src/core/derived_stats.h
// Keep these in sync when adding/removing attribute keys.

enum AttributeKind {
    AttributeKindPrimary = 0,
    AttributeKindDerived = 1,
};

struct AttributeDef {
    const char* key;
    const char* display_name;
    AttributeKind kind;
    bool is_float;
};

inline const std::vector<AttributeDef>& AttributeRegistry() {
    static const std::vector<AttributeDef> registry = {
        // Primary
        {"str", "Strength", AttributeKindPrimary, false},
        {"dex", "Dexterity", AttributeKindPrimary, false},
        {"int", "Intelligence", AttributeKindPrimary, false},
        {"wis", "Wisdom", AttributeKindPrimary, false},
        {"per", "Perception", AttributeKindPrimary, false},

        // Derived
        {"health_max","Health Max", AttributeKindDerived, false},
        {"health_regen","Health Regen", AttributeKindDerived, true},
        {"energy_max","Energy Max", AttributeKindDerived, false},
        {"energy_regen","Energy Regen", AttributeKindDerived, true},
        {"melee_defense_value","Melee Defense", AttributeKindDerived, false},
        {"ranged_defense_value","Ranged Defense", AttributeKindDerived, false},
        {"magic_defense_value","Magic Defense", AttributeKindDerived, false},
        {"melee_evasion_value","Melee Evasion", AttributeKindDerived, false},
        {"ranged_evasion_value","Ranged Evasion", AttributeKindDerived, false},
        {"magic_evasion_value","Magic Evasion", AttributeKindDerived, false},
        {"melee_hit_value","Melee Hit", AttributeKindDerived, false},
        {"ranged_hit_value","Ranged Hit", AttributeKindDerived, false},
        {"magic_hit_value","Magic Hit", AttributeKindDerived, false},
        {"melee_crit_value","Melee Crit", AttributeKindDerived, false},
        {"ranged_crit_value","Ranged Crit", AttributeKindDerived, false},
        {"magic_crit_value","Magic Crit", AttributeKindDerived, false},
        {"melee_dmg_min","Melee Dmg Min", AttributeKindDerived, false},
        {"melee_dmg_max","Melee Dmg Max", AttributeKindDerived, false},
        {"ranged_dmg_min","Ranged Dmg Min", AttributeKindDerived, false},
        {"ranged_dmg_max","Ranged Dmg Max", AttributeKindDerived, false},
        {"magic_dmg_min","Magic Dmg Min", AttributeKindDerived, false},
        {"magic_dmg_max","Magic Dmg Max", AttributeKindDerived, false},
        {"crit_damage_mult","Crit Damage Mult", AttributeKindDerived, true},
        {"attack_speed_mult","Attack Speed Mult", AttributeKindDerived, true},
        {"movement_speed_mult","Movement Speed Mult", AttributeKindDerived, true},
        {"cooldown_speed_pct","Cooldown Speed %", AttributeKindDerived, true},
        {"skill_damage_boost_pct","Skill Damage Boost %", AttributeKindDerived, true},
        {"buff_duration_pct","Buff Duration %", AttributeKindDerived, true},
        {"debuff_duration_pct","Debuff Duration %", AttributeKindDerived, true},
        {"range_bonus_pct","Range Bonus %", AttributeKindDerived, true},
        {"bonus_damage_flat","Bonus Damage Flat", AttributeKindDerived, false},
        {"cc_chance_value","CC Chance", AttributeKindDerived, false},
        {"cc_resistance_value","CC Resistance", AttributeKindDerived, false},
        {"damage_reduction_flat","Damage Reduction Flat", AttributeKindDerived, false},
    };
    return registry;
}

inline const std::vector<std::pair<std::string,std::string>>& AttributeList() {
    static const std::vector<std::pair<std::string, std::string>> list = [] {
        std::vector<std::pair<std::string, std::string>> tmp;
        tmp.reserve(AttributeRegistry().size());
        for (const auto& def : AttributeRegistry()) {
            tmp.push_back({def.key, def.display_name});
        }
        return tmp;
    }();
    return list;
}

inline std::vector<std::string> AttributeDisplayNames() {
    std::vector<std::string> names;
    names.reserve(AttributeRegistry().size());
    for (const auto& def : AttributeRegistry()) {
        names.push_back(def.display_name);
    }
    return names;
}

inline bool IsKnownAttributeKey(const std::string& key) {
    for (const auto& def : AttributeRegistry()) {
        if (key == def.key) return true;
    }
    return false;
}

inline std::string AttributeKeyFromDisplay(const std::string& display) {
    for (const auto& def : AttributeRegistry()) {
        if (display == def.display_name) return def.key;
    }
    return {};
}

inline std::string AttributeDisplayFromKey(const std::string& key) {
    for (const auto& def : AttributeRegistry()) {
        if (key == def.key) return def.display_name;
    }
    return key;
}

}  // namespace gue
