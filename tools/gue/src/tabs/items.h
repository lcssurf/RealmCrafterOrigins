#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

struct ItemAttribute {
    std::string key;
    double      value = 0.0;
};

struct ItemSocketOverride {
    int   id = 0;
    int   item_template_id = 0;
    int   actor_def_id = 0;
    float offset_pos_x = 0.f;
    float offset_pos_y = 0.f;
    float offset_pos_z = 0.f;
    float offset_rot_x = 0.f;
    float offset_rot_y = 0.f;
    float offset_rot_z = 0.f;
    float offset_scale = 1.f;
};

struct ItemTemplate {
    int         id            = 0;
    std::string name;
    int         item_type     = 3; // 0=weapon 1=armor 2=consumable 3=misc
    int         slot_type     = 255;
    int         weapon_damage = 0;
    int         armor_level   = 0;
    int         weapon_dimension = 0; // 0=melee 1=ranged 2=magic
    int         weapon_hands     = 1; // 1=one-hand 2=two-hand (no mechanical effect yet)
    float       weapon_range     = 0.0f; // explicit attack range; 0 = use dimension default
    std::string weapon_kit;        // kit_key reference, "" = none
    int         max_stack     = 1;
    int         item_value    = 0;
    bool        stackable     = false;
    std::string model_path;
    float       model_scale   = 1.f;
    std::string socket_name;
    std::vector<ItemSocketOverride> overrides;
    std::vector<ItemAttribute> attributes;
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
    bool LoadItemAttributes(sqlite3* db, ItemTemplate& t);
    bool SaveItemAttributes(sqlite3* db, const ItemTemplate& t);
    bool LoadItemOverrides(sqlite3* db, ItemTemplate& t);
    bool SaveItemOverrides(sqlite3* db, const ItemTemplate& t);
    bool LoadSocketVocabulary(sqlite3* db);
    bool LoadActorDefs(sqlite3* db);

    std::vector<ItemTemplate> items_;
    std::vector<WeaponKitOption> weapon_kit_options_;
    int          selected_  = -1;
    bool         dirty_     = false;
    bool         needFetch_ = true;
    char         statusMsg_[256] = {};
    ItemTemplate editing_;
    ItemTemplate newItem_;
    bool         showNew_   = false;
    std::vector<std::string> socketVocab_;
    std::vector<std::pair<int, std::string>> actorDefOptions_;
};

} // namespace gue
