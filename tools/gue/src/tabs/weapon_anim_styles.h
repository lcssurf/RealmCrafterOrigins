#pragma once

#include <string>
#include <vector>
#include "sqlite3.h"

namespace gue {

// A PHYSICAL grip/pose archetype ("sword_1h", "staff", "bow", "dagger",
// "wand"...) — INDEPENDENT of WeaponKit (which pool of skills a weapon
// grants) and of weapon_dimension/weapon_hands on item_templates (damage and
// range mechanics). Many items with different kits, different dimensions,
// even different visuals can share one style: every 1-handed sword uses
// "sword_1h" regardless of which skills or how much damage it deals.
//
// This mirrors WeaponKitRow structurally on purpose (same governed-catalog
// pattern: a stable key + display name + soft-delete via `enabled`) so the
// Animation Vocabulary editor's "Add Weapon-Specific Binding" combo and the
// Actor Def preview's "Simulate weapon equipped" combo can read this table
// exactly like they'd read weapon_kits — see docs/TECH_DEBT.md for the
// "weapon_type mixed empunhadura+dimensão" precedent this table exists to
// avoid repeating with kit_key.
struct WeaponAnimStyleRow {
    int         id = 0;
    std::string style_key;
    std::string display_name;
    std::string description;
    bool        enabled = true;
};

class WeaponAnimStylesTab {
public:
    void Draw(sqlite3* db);

private:
    void FetchStyles(sqlite3* db);

    bool ValidateStyle(sqlite3* db,
                       const WeaponAnimStyleRow& row,
                       bool is_new,
                       std::string* out_error);

    bool SaveStyle(sqlite3* db);
    bool DeleteStyleSoft(sqlite3* db, int style_id);

    void DrawList(sqlite3* db);
    void DrawEditor(sqlite3* db);
    void DrawNewStyleForm(sqlite3* db);

    void SetStatus(const char* fmt, ...);

    std::vector<WeaponAnimStyleRow> styles_;

    int                selected_ = -1;
    WeaponAnimStyleRow editing_style_;
    bool               dirty_style_ = false;

    bool               show_new_form_ = false;
    WeaponAnimStyleRow new_style_;

    bool need_fetch_styles_ = true;

    std::string select_style_key_after_fetch_;
    char        status_msg_[256] = {};
};

} // namespace gue
