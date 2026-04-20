#pragma once
#include <string>
#include <array>
#include <cstdint>
#include <functional>

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
    // Slot layout: 0-13 equip, 14-45 backpack (32 slots = 8×4)
    static constexpr int kTotalSlots    = 46;
    static constexpr int kEquipSlots    = 14;
    static constexpr int kBackpackStart = 14;
    static constexpr int kBackpackSlots = 32;
    static constexpr int kBackpackCols  = 8;
    static constexpr int kBackpackRows  = 4;

    static constexpr const char* kEquipSlotNames[kEquipSlots] = {
        "Weapon","Shield","Hat","Chest","Hands",
        "Belt","Legs","Feet","Ring 1","Ring 2",
        "Ring 3","Ring 4","Amulet 1","Amulet 2"
    };

    void SetSlot(uint8_t slot, uint16_t item_id, uint8_t qty, uint8_t dur,
                 const std::string& name, uint8_t item_type,
                 uint8_t slot_type, int16_t weapon_damage, int16_t armor_level);
    void Clear();

    void Render(int screenW, int screenH);

    // Called when the user drags one slot onto another.
    std::function<void(int src, int dst)> on_swap;

    // Called when the user right-clicks a bag slot (use/equip).
    std::function<void(int slot)> on_use;

    bool visible = false;

private:
    std::array<InventoryItem, kTotalSlots> slots_ = {};

    bool canEquipInSlot(int equip_slot, const InventoryItem& item) const;
    void drawTooltip(const InventoryItem& item, int equip_slot_hint) const;
};

} // namespace rco::ui
