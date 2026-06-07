#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>

namespace gue {

struct CombatAbilityTemplate {
    int         id = 0;
    std::string name;
    std::string description;
    std::string family = "melee_special";
    std::string category = "damage";
    std::string resource_type = "none";
    int         resource_cost = 0;
    int         cooldown_ms = 2000;
    float       range_min = 0.f;
    float       range_max = 2.5f;
    int         windup_ms = 700;
    int         impact_delay_ms = 0;
    int         recover_ms = 400;
    int         parry_window_ms = 200;
    bool        interruptible = true;
    int         base_damage_min = 0;
    int         base_damage_max = 0;
    std::string damage_stat_scale_json;
    float       armor_pierce_pct = 0.f;
    std::string crit_policy_json;
    std::string telegraph_type = "ring_close";
    float       telegraph_radius = 2.5f;
    std::string telegraph_color_rgba = "1,0.2,0.2,0.75";
    std::string action_windup = "Attack";
    std::string action_impact = "Attack";
    std::string action_recover = "Idle";
    bool        allow_action_override = false;
    std::string allowed_action_tags_json;
    int         vfx_id_windup = 0;
    int         vfx_id_impact = 0;
    int         sfx_id_windup = 0;
    int         sfx_id_impact = 0;
    std::string vfx_path_windup;
    std::string vfx_path_impact;
    std::string sfx_path_windup;
    std::string sfx_path_impact;
    int         mastery_xp_per_use = 10;
    int         mastery_max_level = 10;
    std::string mastery_xp_curve_type = "irregular";
    int         mastery_xp_curve_base = 40;
    float       mastery_xp_curve_exponent = 2.0f;
    float       mastery_xp_irregularity = 0.5f;
    float       mastery_primary_bonus_per_lvl = 0.03f;
    float       mastery_cooldown_redux_per_lvl = 0.01f;
    bool        enabled = true;
};

struct NPCAbilityLoadoutEntry {
    int         id = 0;
    int         npc_spawn_id = 0;
    int         actor_def_id = 0;
    int         ability_id = 0;
    int         priority = 100;
    int         weight = 100;
    float       min_distance = 0.f;
    float       max_distance = 0.f;
    float       min_target_hp_pct = 0.f;
    float       max_target_hp_pct = 100.f;
    std::string phase_tag;
    std::string condition_lua;
    bool        enabled = true;
};

struct NPCCombatProfile {
    int         id = 0;
    std::string name = "default_profile";
    int         global_gcd_ms = 450;
    int         decision_tick_ms = 250;
    std::string aggro_style = "default";
    bool        allow_chain_cast = false;
    int         max_consecutive_specials = 1;
    bool        enabled = true;
};

struct NPCProfileBinding {
    int  id = 0;
    int  npc_spawn_id = 0;
    int  actor_def_id = 0;
    int  profile_id = 0;
    bool enabled = true;
};

struct NPCSpawnOption {
    int         id = 0;
    std::string name;
    std::string area_name;
    std::string label;
};

struct ActorDefOption {
    int         id = 0;
    std::string name;
    std::string label;
};

struct ProfileOption {
    int         id = 0;
    std::string name;
    std::string label;
};

struct ProgressionDefaults {
    int         xp_per_use = 10;
    int         max_level = 10;
    std::string xp_curve_type = "irregular";
    int         xp_curve_base = 40;
    float       xp_curve_exponent = 2.0f;
    float       xp_irregularity = 0.5f;
    float       damage_bonus_per_level = 0.03f;
    float       cooldown_redux_per_level = 0.01f;
};

class CombatAbilitiesTab {
public:
    void Draw(sqlite3* db);

private:
    void EnsureTables(sqlite3* db);
    void FetchAll(sqlite3* db);
    void FetchAbilities(sqlite3* db);
    void FetchLoadouts(sqlite3* db);
    void FetchProfiles(sqlite3* db);
    void FetchProfileBindings(sqlite3* db);
    void FetchNPCSpawns(sqlite3* db);
    void FetchActorDefs(sqlite3* db);
    void LoadDefaultsIfNeeded(sqlite3* db);

    bool SaveAbility(sqlite3* db, CombatAbilityTemplate& row);
    bool DeleteAbility(sqlite3* db, int ability_id);
    bool SaveLoadout(sqlite3* db, NPCAbilityLoadoutEntry& row);
    bool DeleteLoadout(sqlite3* db, int loadout_id);
    bool SaveProfile(sqlite3* db, NPCCombatProfile& row);
    bool DeleteProfile(sqlite3* db, int profile_id);
    bool SaveProfileBinding(sqlite3* db, NPCProfileBinding& row);
    bool DeleteProfileBinding(sqlite3* db, int binding_id);

    bool ValidateAbility(sqlite3* db, const CombatAbilityTemplate& row, bool is_new, std::string* out_error) const;
    bool ValidateLoadout(sqlite3* db, const NPCAbilityLoadoutEntry& row, std::string* out_error) const;
    bool ValidateProfile(sqlite3* db, const NPCCombatProfile& row, bool is_new, std::string* out_error) const;
    bool ValidateProfileBinding(sqlite3* db, const NPCProfileBinding& row, std::string* out_error) const;

    const char* AbilityNameByID(int ability_id) const;
    const char* SpawnLabelByID(int spawn_id) const;
    const char* ActorDefLabelByID(int actor_def_id) const;
    const char* ProfileNameByID(int profile_id) const;

    void SetStatus(const char* fmt, ...);

    bool tables_ensured_ = false;
    bool defaults_loaded_ = false;
    bool need_fetch_ = true;

    int select_ability_after_fetch_ = 0;
    int select_loadout_after_fetch_ = 0;
    int select_profile_after_fetch_ = 0;
    int select_profile_binding_after_fetch_ = 0;

    std::vector<CombatAbilityTemplate> abilities_;
    std::vector<NPCAbilityLoadoutEntry> loadouts_;
    std::vector<NPCCombatProfile> profiles_;
    std::vector<NPCProfileBinding> profile_bindings_;
    std::vector<NPCSpawnOption> npc_spawns_;
    std::vector<ActorDefOption> actor_defs_;
    std::vector<ProfileOption> profile_options_;

    int selected_ability_ = -1;
    int selected_loadout_ = -1;
    int selected_profile_ = -1;
    int selected_profile_binding_ = -1;

    bool show_new_ability_ = false;
    bool show_new_loadout_ = false;
    bool show_new_profile_ = false;
    bool show_new_profile_binding_ = false;

    bool dirty_ability_ = false;
    bool dirty_loadout_ = false;
    bool dirty_profile_ = false;
    bool dirty_profile_binding_ = false;

    CombatAbilityTemplate new_ability_;
    CombatAbilityTemplate editing_ability_;
    NPCAbilityLoadoutEntry new_loadout_;
    NPCAbilityLoadoutEntry editing_loadout_;
    NPCCombatProfile new_profile_;
    NPCCombatProfile editing_profile_;
    NPCProfileBinding new_profile_binding_;
    NPCProfileBinding editing_profile_binding_;

    char status_msg_[256] = {};
    ProgressionDefaults defaults_;
};

} // namespace gue
