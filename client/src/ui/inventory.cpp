#include "inventory.h"
#include "ui_texture.h"
#include <imgui.h>
#include <cstdio>
#include <algorithm>

namespace rco::ui {

constexpr const char* Inventory::kEquipSlotNames[kEquipSlots];

static const char* kSlotIcons[Inventory::kEquipSlots] = {
    "Weapon.bmp", "Shield.bmp", "Hat.bmp",  "Chest.bmp",
    "Hand.bmp",   "Belt.bmp",   "Legs.bmp", "Feet.bmp",
    "Ring.bmp",   "Ring.bmp",   "Ring.bmp", "Ring.bmp",
    "Amulet.bmp", "Amulet.bmp"
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool Inventory::canEquipInSlot(int equip_slot, const InventoryItem& item) const {
    if (item.empty()) return true;
    uint8_t st = item.slot_type;
    if (st == 255) return false;               // bag-only
    if (st <= 7)   return equip_slot == st;
    if (st == 8)   return equip_slot >= 8 && equip_slot <= 11;  // rings
    if (st == 9)   return equip_slot == 12 || equip_slot == 13; // amulets
    return false;
}

void Inventory::drawTooltip(const InventoryItem& item, int equip_slot_hint) const {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(item.name.c_str());
    ImGui::Separator();
    if (item.weapon_damage > 0)
        ImGui::Text("Damage: %d", (int)item.weapon_damage);
    if (item.armor_level > 0)
        ImGui::Text("Armor:  %d", (int)item.armor_level);
    ImGui::TextDisabled("Durability: %d / 100", (int)item.durability);
    if (item.quantity > 1)
        ImGui::TextDisabled("Quantity: %d", (int)item.quantity);
    if (item.durability == 0)
        ImGui::TextColored({1.f,0.3f,0.3f,1.f}, "BROKEN — cannot equip");
    ImGui::EndTooltip();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Inventory::SetSlot(uint8_t slot, uint16_t item_id, uint8_t qty, uint8_t dur,
                        const std::string& name, uint8_t item_type,
                        uint8_t slot_type, int16_t weapon_damage, int16_t armor_level) {
    if (slot >= kTotalSlots) return;
    slots_[slot] = { item_id, qty, dur, item_type, slot_type, weapon_damage, armor_level, name };
}

void Inventory::Clear() { slots_ = {}; }

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

static void pushSlotColors(bool occupied, uint8_t item_type, bool broken) {
    if (!occupied) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.10f,0.10f,0.10f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.18f,0.18f,0.18f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.22f,0.22f,0.22f,1.f});
        return;
    }
    if (broken) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.35f,0.10f,0.10f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.45f,0.15f,0.15f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.50f,0.18f,0.18f,1.f});
        return;
    }
    ImVec4 base, hov, act;
    switch (item_type) {
        case 0:  base={0.50f,0.28f,0.10f,1.f}; hov={0.62f,0.38f,0.14f,1.f}; act={0.68f,0.42f,0.16f,1.f}; break; // weapon — brown
        case 1:  base={0.15f,0.28f,0.52f,1.f}; hov={0.22f,0.38f,0.65f,1.f}; act={0.25f,0.42f,0.70f,1.f}; break; // armor  — blue
        case 2:  base={0.16f,0.42f,0.16f,1.f}; hov={0.22f,0.55f,0.22f,1.f}; act={0.25f,0.60f,0.25f,1.f}; break; // potion — green
        default: base={0.26f,0.26f,0.26f,1.f}; hov={0.34f,0.34f,0.34f,1.f}; act={0.38f,0.38f,0.38f,1.f}; break;
    }
    ImGui::PushStyleColor(ImGuiCol_Button,        base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
}

void Inventory::Render(int screenW, int screenH) {
    if (!visible) return;

    constexpr float kWinW = 560.f;
    constexpr float kWinH = 380.f;
    const float wx = (screenW - kWinW) * 0.5f;
    const float wy = (screenH - kWinH) * 0.5f;

    ImGui::SetNextWindowPos({wx, wy}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({kWinW, kWinH}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    if (!ImGui::Begin("Inventory  [I]", &visible,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    // ---- Equipment panel (left) ----
    ImGui::BeginChild("##equip", {175.f, 0.f}, true);
    ImGui::TextDisabled("Equipment");
    ImGui::Separator();
    ImGui::Spacing();

    constexpr float kEquipH = 22.f;

    for (int i = 0; i < kEquipSlots; ++i) {
        const auto& it = slots_[i];
        bool broken = !it.empty() && it.durability == 0;

        pushSlotColors(!it.empty(), it.item_type, broken);
        ImGui::PushID(i);

        char lbl[48];
        if (it.empty())
            snprintf(lbl, sizeof(lbl), "##eq_%d", i);
        else
            snprintf(lbl, sizeof(lbl), "%s##eq", it.name.c_str());

        ImGui::Button(lbl, {155.f, kEquipH});
        ImGui::PopStyleColor(3);

        // Draw slot icon on empty slots.
        if (it.empty()) {
            ImTextureID icon = g_tex.GUI(kSlotIcons[i]);
            if (icon) {
                ImVec2 p = ImGui::GetItemRectMin();
                ImVec2 sz = ImGui::GetItemRectSize();
                float isz = sz.y - 2.f;
                ImGui::GetWindowDrawList()->AddImage(
                    icon, {p.x + 2.f, p.y + 1.f}, {p.x + 2.f + isz, p.y + 1.f + isz},
                    {0,0},{1,1}, IM_COL32(255,255,255,120));
            }
            // Also draw slot name as dim text.
            ImVec2 p = ImGui::GetItemRectMin();
            ImVec2 sz = ImGui::GetItemRectSize();
            ImGui::GetWindowDrawList()->AddText(
                {p.x + sz.y + 4.f, p.y + (sz.y - ImGui::GetTextLineHeight()) * 0.5f},
                IM_COL32(180, 180, 180, 160), kEquipSlotNames[i]);
        }

        // Drag source
        if (!it.empty() && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("INV", &i, sizeof(int));
            ImGui::TextUnformatted(it.name.c_str());
            ImGui::EndDragDropSource();
        }
        // Drop target
        if (ImGui::BeginDragDropTarget()) {
            if (auto* p = ImGui::AcceptDragDropPayload("INV")) {
                int src = *static_cast<const int*>(p->Data);
                if (canEquipInSlot(i, slots_[src])) {
                    std::swap(slots_[i], slots_[src]);
                    if (on_swap) on_swap(src, i);
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (!it.empty() && ImGui::IsItemHovered())
            drawTooltip(it, i);

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Backpack grid (right) ----
    ImGui::BeginChild("##bag", {0.f, 0.f}, true);
    int bagUsed = 0;
    for (int i = kBackpackStart; i < kTotalSlots; ++i)
        if (!slots_[i].empty()) bagUsed++;
    ImGui::TextDisabled("Backpack  (%d / %d)", bagUsed, kBackpackSlots);
    ImGui::Separator();
    ImGui::Spacing();

    constexpr float kCellSz = 48.f;
    constexpr float kPad    = 3.f;

    for (int row = 0; row < kBackpackRows; ++row) {
        for (int col = 0; col < kBackpackCols; ++col) {
            if (col > 0) ImGui::SameLine(0.f, kPad);

            int slot = kBackpackStart + row * kBackpackCols + col;
            const auto& it = slots_[slot];
            bool broken = !it.empty() && it.durability == 0;

            pushSlotColors(!it.empty(), it.item_type, broken);
            ImGui::PushID(slot);

            char lbl[32] = {};
            if (!it.empty()) {
                if (it.quantity > 1)
                    snprintf(lbl, sizeof(lbl), "%.8s\nx%d", it.name.c_str(), (int)it.quantity);
                else
                    snprintf(lbl, sizeof(lbl), "%.10s", it.name.c_str());
            }

            ImGui::Button(lbl, {kCellSz, kCellSz});
            ImGui::PopStyleColor(3);

            // Empty slot icon.
            if (it.empty()) {
                ImTextureID icon = g_tex.GUI("EmptySlot.bmp");
                if (icon) {
                    ImVec2 p = ImGui::GetItemRectMin();
                    ImGui::GetWindowDrawList()->AddImage(
                        icon, p, {p.x + kCellSz, p.y + kCellSz},
                        {0,0},{1,1}, IM_COL32(255,255,255,50));
                }
            }

            // Right-click: use / auto-equip
            if (!it.empty() && ImGui::IsItemHovered()
                    && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if (on_use) on_use(slot);
            }

            // Drag source
            if (!it.empty() && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("INV", &slot, sizeof(int));
                ImGui::TextUnformatted(it.name.c_str());
                if (it.quantity > 1)
                    ImGui::TextDisabled("x%d", (int)it.quantity);
                ImGui::EndDragDropSource();
            }
            // Drop target (any item can go to backpack)
            if (ImGui::BeginDragDropTarget()) {
                if (auto* p = ImGui::AcceptDragDropPayload("INV")) {
                    int src = *static_cast<const int*>(p->Data);
                    std::swap(slots_[src], slots_[slot]);
                    if (on_swap) on_swap(src, slot);
                }
                ImGui::EndDragDropTarget();
            }

            if (!it.empty() && ImGui::IsItemHovered())
                drawTooltip(it, -1);

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace rco::ui
