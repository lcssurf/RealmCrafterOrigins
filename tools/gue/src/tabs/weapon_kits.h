#pragma once

#include <string>
#include <vector>
#include "sqlite3.h"

namespace gue {

struct WeaponKitRow {
    int         id = 0;
    std::string kit_key;
    std::string display_name;
    std::string description;
    bool        enabled = true;
};

struct WeaponKitAbilityRow {
    int ability_id = 0;
    int slot_index = 0;
    bool enabled = true;
};

struct AbilityOption {
    int         id = 0;
    std::string name;
};

class WeaponKitsTab {
public:
    void Draw(sqlite3* db);

private:
    void FetchKits(sqlite3* db);
    void FetchAbilityOptions(sqlite3* db);
    void LoadKitAbilities(sqlite3* db, int kit_id);

    bool ValidateKit(sqlite3* db,
                     const WeaponKitRow& row,
                     bool is_new,
                     std::string* out_error);

    bool SaveKit(sqlite3* db);
    bool DeleteKitSoft(sqlite3* db, int kit_id);

    void DrawList(sqlite3* db);
    void DrawEditor(sqlite3* db);
    void DrawAbilitiesEditor();
    void DrawNewKitForm(sqlite3* db);

    void SetStatus(const char* fmt, ...);

    std::vector<WeaponKitRow>           kits_;
    std::vector<WeaponKitAbilityRow>    editing_abilities_;
    std::vector<AbilityOption>          ability_options_;

    int          selected_ = -1;
    WeaponKitRow editing_kit_;
    bool         dirty_kit_ = false;
    bool         dirty_abilities_ = false;

    bool         show_new_form_ = false;
    WeaponKitRow new_kit_;

    bool need_fetch_kits_ = true;
    bool need_fetch_abilities_ = true;

    std::string select_kit_key_after_fetch_;
    char        status_msg_[256] = {};
};

} // namespace gue

