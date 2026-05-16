#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

struct ItemTemplate {
    int         id            = 0;
    std::string name;
    int         item_type     = 3; // 0=weapon 1=armor 2=consumable 3=misc
    int         slot_type     = 255;
    int         weapon_damage = 0;
    int         armor_level   = 0;
    int         weapon_type   = 0; // 1=one-hand 2=two-hand 3=ranged
    std::string weapon_kit;        // kit_key reference, "" = none
    int         max_stack     = 1;
    int         item_value    = 0;
    bool        stackable     = false;
};

class ItemsTab {
public:
    void Draw(sqlite3* db);

private:
    struct WeaponKitOption {
        std::string kit_key;
        std::string display_name;
    };

    void Fetch(sqlite3* db);
    void FetchWeaponKitOptions(sqlite3* db);
    bool DrawFields(ItemTemplate& t);
    bool Save(sqlite3* db, ItemTemplate& t);
    bool Delete(sqlite3* db, int id);

    std::vector<ItemTemplate> items_;
    std::vector<WeaponKitOption> weapon_kit_options_;
    int          selected_  = -1;
    bool         dirty_     = false;
    bool         needFetch_ = true;
    char         statusMsg_[256] = {};
    ItemTemplate editing_;
    ItemTemplate newItem_;
    bool         showNew_   = false;
};

} // namespace gue
