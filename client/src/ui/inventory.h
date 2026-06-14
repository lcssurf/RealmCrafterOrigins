#pragma once
#include <string>
#include <array>
#include <cstdint>
#include <functional>
#include "game_state.h"

namespace rco::ui {

struct InventoryItem {
    uint16_t item_id      = 0;
    uint8_t  quantity     = 0;
    uint8_t  durability   = 0;
    uint8_t  item_type    = 0;  // 0=weapon 1=armor 2=consumable 3=misc
    uint8_t  slot_type    = 255; // 0-9=equip slot, 255=bag-only
    int16_t  weapon_damage = 0;
    int16_t  armor_level   = 0;
    std::string name;

    bool empty() const { return item_id == 0; }
};

class Inventory {
public:
    // Slot layout: 0-13 equip, 14-45 backpack (32 slots = 4×8)
    static constexpr int kTotalSlots    = 46;
    static constexpr int kEquipSlots    = 14;
    static constexpr int kBackpackStart = 14;
    static constexpr int kBackpackSlots = 32;
    static constexpr int kBackpackCols  = 4;
    static constexpr int kBackpackRows  = 8;

    static constexpr const char* kEquipSlotNames[kEquipSlots] = {
        "Weapon","Shield","Hat","Chest","Hands",
        "Belt","Legs","Feet","Ring 1","Ring 2",
        "Ring 3","Ring 4","Amulet 1","Amulet 2"
    };

    void SetSlot(uint8_t slot, uint16_t item_id, uint8_t qty, uint8_t dur,
                 const std::string& name, uint8_t item_type,
                 uint8_t slot_type, int16_t weapon_damage, int16_t armor_level);
    void Clear();

    // Renders both sub-windows (each checks its own visibility flag).
    void Render(int screenW, int screenH, const rco::PlayerState& player);

    const InventoryItem& GetSlot(int idx) const { return slots_[idx]; }

    // Called when the user drags one slot onto another.
    std::function<void(int src, int dst)> on_swap;

    // Called when the user right-clicks a bag slot (use/equip).
    std::function<void(int slot)> on_use;

    // Called when user applies pending primary stat points.
    std::function<void(uint8_t stat_id, uint8_t amount)> on_distribute;

    // Called when user confirms build reset.
    std::function<void()> on_respec;

    // Visibility flags — 'I' toggles bag_visible, 'C' toggles char_visible.
    bool    bag_visible  = false;
    bool    char_visible = false;

    // Gold (updated from server packets).
    int64_t gold = 0;

    // Player stats displayed in the character sheet.
    std::string stat_name;
    std::string stat_race;
    std::string stat_class;
    uint16_t    stat_level   = 1;
    int32_t     stat_hp      = 0;
    int32_t     stat_hp_max  = 0;
    int32_t     stat_mp      = 0;
    int32_t     stat_mp_max  = 0;
    int32_t     stat_sp      = 0;
    int32_t     stat_sp_max  = 0;
    uint32_t    stat_xp      = 0;
    uint32_t    stat_xp_current_level = 0;
    uint32_t    stat_xp_next = 100;

    void ResetPreview();
    int32_t TotalAllocated() const;
    int32_t UnspentRemaining(const rco::PlayerState& player) const;
    bool HasPreviewChanges() const;

private:
    std::array<InventoryItem, kTotalSlots> slots_ = {};

    bool canEquipInSlot(int equip_slot, const InventoryItem& item) const;
    void drawTooltip(const InventoryItem& item) const;
    void drawEquipSlot(int slot_index, float sz);

    void RenderBag(int screenW, int screenH);
    void RenderCharacter(int screenW, int screenH, const rco::PlayerState& player);
    void DrawStatRow(const char* name, int32_t base, int32_t effective, int32_t* delta_ptr,
                     const rco::PlayerState& player);

    int32_t preview_str_delta = 0;
    int32_t preview_dex_delta = 0;
    int32_t preview_int_delta = 0;
    int32_t preview_wis_delta = 0;
    int32_t preview_per_delta = 0;
    bool previous_char_visible = false;
};

} // namespace rco::ui
