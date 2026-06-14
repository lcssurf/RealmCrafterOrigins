#include "inventory.h"
#include "ui_texture.h"
#include "util.h"
#include "core/derived_stats.h"
#include <imgui.h>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <string>

namespace rco::ui {

constexpr const char* Inventory::kEquipSlotNames[kEquipSlots];

// Icon filenames parallel to kEquipSlots (0-13)
static const char* kSlotIcons[Inventory::kEquipSlots] = {
    "Weapon.png", "Shield.bmp", "Hat.png",  "Chest.bmp",
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
    if (st == 255) return false;
    if (st <= 7)   return equip_slot == st;
    if (st == 8)   return equip_slot >= 8 && equip_slot <= 11;
    if (st == 9)   return equip_slot == 12 || equip_slot == 13;
    return false;
}

void Inventory::drawTooltip(const InventoryItem& item) const {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(item.name.c_str());
    ImGui::Separator();
    if (item.weapon_damage > 0) ImGui::Text("Damage: %d", (int)item.weapon_damage);
    if (item.armor_level   > 0) ImGui::Text("Armor:  %d", (int)item.armor_level);
    ImGui::TextDisabled("Durability: %d / 100", (int)item.durability);
    if (item.quantity > 1)      ImGui::TextDisabled("Quantity: %d", (int)item.quantity);
    if (item.durability == 0)
        ImGui::TextColored({1.f,0.3f,0.3f,1.f}, "BROKEN \xe2\x80\x94 cannot equip");
    ImGui::EndTooltip();
}

void Inventory::SetSlot(uint8_t slot, uint16_t item_id, uint8_t qty, uint8_t dur,
                        const std::string& name, uint8_t item_type,
                        uint8_t slot_type, int16_t weapon_damage, int16_t armor_level) {
    if (slot >= kTotalSlots) return;
    slots_[slot] = { item_id, qty, dur, item_type, slot_type, weapon_damage, armor_level, name };
}

void Inventory::Clear() { slots_ = {}; }

void Inventory::ResetPreview() {
    preview_str_delta = 0;
    preview_dex_delta = 0;
    preview_int_delta = 0;
    preview_wis_delta = 0;
    preview_per_delta = 0;
}

int32_t Inventory::TotalAllocated() const {
    return preview_str_delta + preview_dex_delta + preview_int_delta +
           preview_wis_delta + preview_per_delta;
}

int32_t Inventory::UnspentRemaining(const rco::PlayerState& player) const {
    return player.unspent_stat_points - TotalAllocated();
}

bool Inventory::HasPreviewChanges() const {
    return TotalAllocated() > 0;
}

void Inventory::Render(int screenW, int screenH, const rco::PlayerState& player) {
    RenderBag(screenW, screenH);
    RenderCharacter(screenW, screenH, player);
    if (previous_char_visible && !char_visible) {
        ResetPreview();
    }
    previous_char_visible = char_visible;
}

// ---------------------------------------------------------------------------
// Shared slot-color push
// ---------------------------------------------------------------------------

static void pushSlotColors(bool occupied, uint8_t item_type, bool broken) {
    if (!occupied) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.08f,0.08f,0.08f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.16f,0.16f,0.16f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.20f,0.20f,0.20f,1.f});
        return;
    }
    if (broken) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.35f,0.10f,0.10f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.45f,0.15f,0.15f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.50f,0.18f,0.18f,1.f});
        return;
    }
    ImVec4 b,h,a;
    switch (item_type) {
        case 0: b={0.50f,0.28f,0.10f,1.f};h={0.62f,0.38f,0.14f,1.f};a={0.68f,0.42f,0.16f,1.f};break;
        case 1: b={0.15f,0.28f,0.52f,1.f};h={0.22f,0.38f,0.65f,1.f};a={0.25f,0.42f,0.70f,1.f};break;
        case 2: b={0.16f,0.42f,0.16f,1.f};h={0.22f,0.55f,0.22f,1.f};a={0.25f,0.60f,0.25f,1.f};break;
        default:b={0.26f,0.26f,0.26f,1.f};h={0.34f,0.34f,0.34f,1.f};a={0.38f,0.38f,0.38f,1.f};break;
    }
    ImGui::PushStyleColor(ImGuiCol_Button,b);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,h);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,a);
}

// ---------------------------------------------------------------------------
// Draw icon (empty slot) or item text overlay (occupied slot)
// ---------------------------------------------------------------------------

static void drawEquipSlotContent(const InventoryItem& it, int slotIdx,
                                 ImVec2 pos, float sz) {
    auto* dl = ImGui::GetWindowDrawList();
    float th = ImGui::GetTextLineHeight();

    if (it.empty()) {
        // Icon, centered, semi-transparent
        ImTextureID icon = g_tex.GUI(kSlotIcons[slotIdx]);
        if (icon) {
            constexpr float kIconInset = 6.f;
            dl->AddImage(icon,
                {pos.x + kIconInset,      pos.y + kIconInset},
                {pos.x + sz - kIconInset, pos.y + sz - kIconInset},
                {0,0},{1,1}, IM_COL32(255,255,255,100));
        }
    } else {
        // Item name (top-left, truncated)
        char trunc[14];
        std::snprintf(trunc, sizeof(trunc), "%.11s", it.name.c_str());
        dl->AddText({pos.x+3.f, pos.y+3.f}, IM_COL32(235,225,200,230), trunc);

        // Durability / broken (bottom-left)
        if (it.durability == 0) {
            dl->AddText({pos.x+3.f, pos.y+sz-th-3.f}, IM_COL32(255,70,70,220), "BROKEN");
        } else {
            char dur[8];
            std::snprintf(dur, sizeof(dur), "%d%%", (int)it.durability);
            dl->AddText({pos.x+3.f, pos.y+sz-th-3.f}, IM_COL32(140,140,130,180), dur);
        }
    }
}

// ---------------------------------------------------------------------------
// One equip-slot button with drag/drop and tooltip.
// Call with cursor already positioned.
// ---------------------------------------------------------------------------

void Inventory::drawEquipSlot(int si, float sz) {
    const auto& it     = slots_[si];
    bool        broken = !it.empty() && it.durability == 0;

    pushSlotColors(!it.empty(), it.item_type, broken);
    ImGui::PushID(si);
    ImGui::Button("##es", {sz, sz});
    ImGui::PopStyleColor(3);

    drawEquipSlotContent(it, si, ImGui::GetItemRectMin(), sz);

    if (!it.empty() && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("INV", &si, sizeof(int));
        ImGui::TextUnformatted(it.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (auto* pay = ImGui::AcceptDragDropPayload("INV")) {
            int src = *static_cast<const int*>(pay->Data);
            if (canEquipInSlot(si, slots_[src])) {
                std::swap(slots_[src], slots_[si]);
                if (on_swap) on_swap(src, si);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered()) {
        if (!it.empty()) drawTooltip(it);
        else {
            ImGui::BeginTooltip();
            ImGui::TextDisabled("%s", kEquipSlotNames[si]);
            ImGui::EndTooltip();
        }
    }
    ImGui::PopID();
}

void Inventory::DrawStatRow(const char* name, int32_t base, int32_t effective, int32_t* delta_ptr,
                            const rco::PlayerState& player) {
    const int32_t bonus = effective - base;
    const int32_t new_val = effective + *delta_ptr;
    if (*delta_ptr > 0) {
        ImGui::Text("%s: %d -> %d", name, effective, new_val);
    } else if (bonus != 0) {
        ImGui::Text("%s: %d (+%d)", name, effective, bonus);
    } else {
        ImGui::Text("%s: %d", name, effective);
    }
    ImGui::SameLine();

    ImGui::BeginDisabled(*delta_ptr <= 0);
    std::string minus_id = std::string("-##") + name;
    if (ImGui::Button(minus_id.c_str())) {
        (*delta_ptr)--;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(UnspentRemaining(player) <= 0);
    std::string plus_id = std::string("+##") + name;
    if (ImGui::Button(plus_id.c_str())) {
        (*delta_ptr)++;
    }
    ImGui::EndDisabled();
}

// ===========================================================================
// BAG WINDOW  (I)
// ===========================================================================

void Inventory::RenderBag(int screenW, int screenH) {
    if (!bag_visible) return;

    constexpr float kWinW = 210.f;
    constexpr float kGap  = 3.f;
    const float     winH  = std::min((float)screenH - 60.f, 430.f);
    const float     wx    = (float)screenW - kWinW - 10.f;

    ImGui::SetNextWindowPos({wx, 40.f}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({kWinW, winH}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f, 8.f});

    if (!ImGui::Begin("Bag##bag", &bag_visible,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();

    const float cw      = ImGui::GetContentRegionAvail().x;
    const float bCellSz = (cw - (kBackpackCols-1)*kGap) / kBackpackCols;

    // Bag grid fills most of the window, leaving room for stats at bottom
    float footerH = ImGui::GetTextLineHeightWithSpacing() * 2.f + 12.f;
    float bagH    = ImGui::GetContentRegionAvail().y - footerH;

    ImGui::BeginChild("##bagGrid", {0.f, bagH > 40.f ? bagH : 40.f}, false, 0);

    for (int row = 0; row < kBackpackRows; ++row) {
        for (int col = 0; col < kBackpackCols; ++col) {
            if (col > 0) ImGui::SameLine(0.f, kGap);

            int         slot = kBackpackStart + row*kBackpackCols + col;
            const auto& it   = slots_[slot];
            bool        broken = !it.empty() && it.durability == 0;

            pushSlotColors(!it.empty(), it.item_type, broken);
            ImGui::PushID(slot);
            ImGui::Button("##bc", {bCellSz, bCellSz});
            ImGui::PopStyleColor(3);

            if (!it.empty()) {
                ImVec2 p  = ImGui::GetItemRectMin();
                auto*  dl = ImGui::GetWindowDrawList();
                float  th = ImGui::GetTextLineHeight();
                char trunc[12];
                std::snprintf(trunc, sizeof(trunc), "%.9s", it.name.c_str());
                dl->AddText({p.x+3.f, p.y+3.f}, IM_COL32(235,225,200,230), trunc);
                if (it.quantity > 1) {
                    char qty[8];
                    std::snprintf(qty, sizeof(qty), "x%d", (int)it.quantity);
                    ImVec2 qsz = ImGui::CalcTextSize(qty);
                    dl->AddText({p.x+bCellSz-qsz.x-3.f, p.y+bCellSz-th-3.f},
                                IM_COL32(180,240,180,220), qty);
                }
            } else {
                ImVec2 p = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    {p.x+bCellSz*0.5f, p.y+bCellSz*0.5f}, 2.f, IM_COL32(80,78,72,100));
            }

            if (!it.empty() && ImGui::IsItemHovered()
                    && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                if (on_use) on_use(slot);

            if (!it.empty() && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("INV", &slot, sizeof(int));
                ImGui::TextUnformatted(it.name.c_str());
                if (it.quantity > 1) ImGui::TextDisabled("x%d", (int)it.quantity);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (auto* pay = ImGui::AcceptDragDropPayload("INV")) {
                    int src = *static_cast<const int*>(pay->Data);
                    std::swap(slots_[src], slots_[slot]);
                    if (on_swap) on_swap(src, slot);
                }
                ImGui::EndDragDropTarget();
            }
            if (!it.empty() && ImGui::IsItemHovered()) drawTooltip(it);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Slot counter + gold
    int bagUsed = 0;
    for (int i = kBackpackStart; i < kTotalSlots; ++i)
        if (!slots_[i].empty()) bagUsed++;

    ImGui::Separator();
    ImGui::TextDisabled("%d / %d slots", bagUsed, kBackpackSlots);
    ImGui::SameLine();
    ImGui::TextColored({1.f,0.90f,0.40f,1.f}, "  \xE2\x97\x8F %lld g", (long long)gold);

    ImGui::End();
}

// ===========================================================================
// CHARACTER SHEET WINDOW  (C)
//   WoW layout — equip columns on sides, silhouette in center, stats below
//
//   Left  (top→bot): Hat(2) Amulet1(12) Amulet2(13) Chest(3) Belt(5) Hands(4) Legs(6)
//   Right (top→bot): Ring1(8) Ring2(9) Ring3(10) Ring4(11) Feet(7) Weapon(0) Shield(1)
// ===========================================================================

static const int kLeftSlots [7] = { 2, 12, 13, 3, 5, 4, 6 };
static const int kRightSlots[7] = { 8,  9, 10, 11, 7, 0, 1 };

static void drawSilhouette(ImDrawList* dl, ImVec2 c, float s) {
    // head
    dl->AddCircle({c.x, c.y - s*52.f}, s*16.f, IM_COL32(190,180,160,90), 24, 1.5f);
    // neck
    dl->AddRectFilled({c.x-s*5.f, c.y-s*36.f},{c.x+s*5.f, c.y-s*28.f}, IM_COL32(170,162,145,75));
    // torso
    dl->AddRectFilled({c.x-s*22.f,c.y-s*28.f},{c.x+s*22.f,c.y+s*22.f}, IM_COL32(170,162,145,75));
    // left arm
    dl->AddRectFilled({c.x-s*36.f,c.y-s*26.f},{c.x-s*23.f,c.y+s*16.f}, IM_COL32(155,148,132,68));
    // right arm
    dl->AddRectFilled({c.x+s*23.f,c.y-s*26.f},{c.x+s*36.f,c.y+s*16.f}, IM_COL32(155,148,132,68));
    // left leg
    dl->AddRectFilled({c.x-s*20.f,c.y+s*22.f},{c.x-s*4.f, c.y+s*62.f}, IM_COL32(155,148,132,68));
    // right leg
    dl->AddRectFilled({c.x+s*4.f, c.y+s*22.f},{c.x+s*20.f,c.y+s*62.f}, IM_COL32(155,148,132,68));
}

void Inventory::RenderCharacter(int screenW, int screenH, const rco::PlayerState& player) {
    if (!char_visible) return;

    constexpr float kWinW    = 400.f;
    constexpr float kSlotSz  = 44.f;
    constexpr float kSlotGap = 4.f;
    constexpr int   kRows    = 7;
    constexpr float kColH    = kRows * (kSlotSz + kSlotGap) - kSlotGap;

    const float winH = std::min((float)screenH - 60.f, 540.f);
    const float wx   = (float)screenW - kWinW - 10.f
                       - (bag_visible ? 220.f : 0.f);

    ImGui::SetNextWindowPos({wx, 40.f}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({kWinW, winH}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.f, 8.f});

    if (!ImGui::Begin("Character##char", &char_visible,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();

    const float cw = ImGui::GetContentRegionAvail().x;

    // ---- Header ----
    {
        float tw = ImGui::CalcTextSize(stat_name.c_str()).x;
        ImGui::SetCursorPosX((cw - tw) * 0.5f);
        ImGui::TextColored({1.f,0.88f,0.55f,1.f}, "%s", stat_name.c_str());
    }
    {
        char sub[96];
        const int display_level = (player.level > 0) ? static_cast<int>(player.level)
                                                     : static_cast<int>(stat_level);
        std::snprintf(sub, sizeof(sub), "Level %d  %s %s",
                      display_level, stat_race.c_str(), stat_class.c_str());
        float tw = ImGui::CalcTextSize(sub).x;
        ImGui::SetCursorPosX((cw - tw) * 0.5f);
        ImGui::TextDisabled("%s", sub);
    }
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##CharacterTabs")) {
        if (ImGui::BeginTabItem("Stats")) {
            const float barW = (cw - 8.f) * 0.5f;

            float hpFill = stat_hp_max > 0 ? static_cast<float>(stat_hp) / static_cast<float>(stat_hp_max) : 0.f;
            char hpLbl[32];
            std::snprintf(hpLbl, sizeof(hpLbl), "%d / %d", stat_hp, stat_hp_max);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4{0.75f,0.18f,0.18f,1.f});
            ImGui::ProgressBar(hpFill, {barW, 16.f}, hpLbl);
            ImGui::PopStyleColor();

            ImGui::SameLine(0.f, 8.f);

            float mpFill = stat_mp_max > 0 ? static_cast<float>(stat_mp) / static_cast<float>(stat_mp_max) : 0.f;
            char mpLbl[32];
            std::snprintf(mpLbl, sizeof(mpLbl), "%d / %d", stat_mp, stat_mp_max);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4{0.18f,0.38f,0.80f,1.f});
            ImGui::ProgressBar(mpFill, {barW, 16.f}, mpLbl);
            ImGui::PopStyleColor();

            ImGui::Spacing();

            float spFill = stat_sp_max > 0 ? static_cast<float>(stat_sp) / static_cast<float>(stat_sp_max) : 0.f;
            char spLbl[32];
            std::snprintf(spLbl, sizeof(spLbl), "SP %d / %d", stat_sp, stat_sp_max);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4{0.22f,0.78f,0.50f,1.f});
            ImGui::ProgressBar(spFill, {cw, 14.f}, spLbl);
            ImGui::PopStyleColor();

            ImGui::Spacing();

            int16_t totalAtk = 0;
            int16_t totalArmor = 0;
            for (int i = 0; i < kEquipSlots; ++i) {
                totalAtk += slots_[i].weapon_damage;
                totalArmor += slots_[i].armor_level;
            }

            ImGui::Columns(2, "##attrCols", false);
            ImGui::SetColumnWidth(0, cw * 0.55f);

            auto attrRow = [](const char* label, ImVec4 col, const char* fmt, ...) {
                ImGui::TextColored(col, "%s", label);
                ImGui::NextColumn();
                char buf[32];
                va_list ap;
                va_start(ap, fmt);
                std::vsnprintf(buf, sizeof(buf), fmt, ap);
                va_end(ap);
                ImGui::TextUnformatted(buf);
                ImGui::NextColumn();
            };

            attrRow("  Attack", {1.00f,0.85f,0.30f,1.f}, "%d", static_cast<int>(totalAtk));
            attrRow("  Armor", {0.60f,0.80f,1.00f,1.f}, "%d", static_cast<int>(totalArmor));
            attrRow("  Gold", {1.00f,0.90f,0.40f,1.f}, "%lld", static_cast<long long>(gold));

            ImGui::Columns(1);
            ImGui::Spacing();

            const float xpFill = ProgressBetweenThresholds(stat_xp, stat_xp_current_level, stat_xp_next);
            const std::string xpValue = AbbreviateNumber(stat_xp);
            const std::string xpNextValue = AbbreviateNumber(stat_xp_next);
            char xpLbl[64];
            std::snprintf(xpLbl, sizeof(xpLbl), "%s / %s", xpValue.c_str(), xpNextValue.c_str());
            ImGui::TextDisabled("XP %.1f%%", xpFill * 100.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4{0.40f,0.72f,0.40f,1.f});
            ImGui::ProgressBar(xpFill, {cw, 12.f}, xpLbl);
            ImGui::PopStyleColor();

            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("PRIMARY STATS - Unspent: %d", UnspentRemaining(player));
            DrawStatRow("Strength", player.primary.STR, player.primary_effective.STR, &preview_str_delta, player);
            DrawStatRow("Dexterity", player.primary.DEX, player.primary_effective.DEX, &preview_dex_delta, player);
            DrawStatRow("Intelligence", player.primary.INT, player.primary_effective.INT, &preview_int_delta, player);
            DrawStatRow("Wisdom", player.primary.WIS, player.primary_effective.WIS, &preview_wis_delta, player);
            DrawStatRow("Perception", player.primary.PER, player.primary_effective.PER, &preview_per_delta, player);

            ImGui::Spacing();
            ImGui::BeginDisabled(!HasPreviewChanges());
            if (ImGui::Button("APPLY")) {
                auto send_delta = [&](uint8_t stat_id, int32_t delta) {
                    if (!on_distribute || delta <= 0) return;
                    int32_t remaining = delta;
                    while (remaining > 0) {
                        const int32_t chunk = std::min<int32_t>(remaining, 200);
                        on_distribute(stat_id, static_cast<uint8_t>(chunk));
                        remaining -= chunk;
                    }
                };
                send_delta(0, preview_str_delta);
                send_delta(1, preview_dex_delta);
                send_delta(2, preview_int_delta);
                send_delta(3, preview_wis_delta);
                send_delta(4, preview_per_delta);
                ResetPreview();
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!HasPreviewChanges());
            if (ImGui::Button("RESET PREVIEW")) {
                ResetPreview();
            }
            ImGui::EndDisabled();

            constexpr int32_t kRespecFreeUntilLevel = 10;
            constexpr int32_t kRespecCostGold = 1000;
            constexpr int32_t kInitialStatValue = 5;
            const bool respec_is_free = static_cast<int32_t>(player.level) <= kRespecFreeUntilLevel;
            const char* respec_label = respec_is_free ? "RESET BUILD (Free)" : "RESET BUILD (1000 gold)";
            ImGui::SameLine();
            if (ImGui::Button(respec_label)) {
                ImGui::OpenPopup("ConfirmRespec");
            }
            if (ImGui::BeginPopupModal("ConfirmRespec", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Are you sure you want to respec?");
                ImGui::Text("All primary stats will reset to %d.", kInitialStatValue);
                if (respec_is_free) {
                    ImGui::Text("This is FREE (you are level %d, free until level %d).",
                                static_cast<int>(player.level), kRespecFreeUntilLevel);
                } else {
                    ImGui::Text("This will cost %d gold.", kRespecCostGold);
                }
                ImGui::Separator();
                if (ImGui::Button("Confirm")) {
                    if (on_respec) on_respec();
                    ResetPreview();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("--- DERIVED STATS ---");

            const rco::stats::PrimaryStats& confirmed = player.primary;

            // NOTE: confirmed_derived comes from the server (PFullStats,
            // includes gear/item bonuses); preview_derived is a local
            // simulation WITHOUT gear. They diverge by the gear contribution.
            // Preview shows the delta direction of spending points. A fully
            // gear-aware preview would need item bonuses sent to the client
            // (future).
            const auto& confirmed_derived = player.derived;

            rco::stats::PrimaryStats preview{
                player.primary_effective.STR + preview_str_delta,
                player.primary_effective.DEX + preview_dex_delta,
                player.primary_effective.INT + preview_int_delta,
                player.primary_effective.WIS + preview_wis_delta,
                player.primary_effective.PER + preview_per_delta,
            };
            const auto preview_derived = rco::stats::ComputeDerivedStats(
                preview, static_cast<int32_t>(player.level), 0, 0);

            const bool has_changes = HasPreviewChanges();

            auto draw_derived_int = [has_changes](const char* name, int32_t conf, int32_t prev) {
                if (conf != prev && has_changes) {
                    ImGui::Text("%s: %d -> %d", name, conf, prev);
                } else {
                    ImGui::Text("%s: %d", name, conf);
                }
            };
            auto draw_derived_float = [has_changes](const char* name, float conf, float prev) {
                if (std::fabs(conf - prev) > 0.001f && has_changes) {
                    ImGui::Text("%s: %.2f -> %.2f", name, conf, prev);
                } else {
                    ImGui::Text("%s: %.2f", name, conf);
                }
            };

            draw_derived_int("HealthMax", confirmed_derived.HealthMax, preview_derived.HealthMax);
            draw_derived_float("HealthRegen", confirmed_derived.HealthRegen, preview_derived.HealthRegen);
            draw_derived_int("EnergyMax", confirmed_derived.EnergyMax, preview_derived.EnergyMax);
            draw_derived_float("EnergyRegen", confirmed_derived.EnergyRegen, preview_derived.EnergyRegen);
            draw_derived_int("MeleeDefenseValue", confirmed_derived.MeleeDefenseValue, preview_derived.MeleeDefenseValue);
            draw_derived_int("RangedDefenseValue", confirmed_derived.RangedDefenseValue, preview_derived.RangedDefenseValue);
            draw_derived_int("MagicDefenseValue", confirmed_derived.MagicDefenseValue, preview_derived.MagicDefenseValue);
            draw_derived_int("MeleeEvasionValue", confirmed_derived.MeleeEvasionValue, preview_derived.MeleeEvasionValue);
            draw_derived_int("RangedEvasionValue", confirmed_derived.RangedEvasionValue, preview_derived.RangedEvasionValue);
            draw_derived_int("MagicEvasionValue", confirmed_derived.MagicEvasionValue, preview_derived.MagicEvasionValue);
            draw_derived_int("MeleeHitValue", confirmed_derived.MeleeHitValue, preview_derived.MeleeHitValue);
            draw_derived_int("RangedHitValue", confirmed_derived.RangedHitValue, preview_derived.RangedHitValue);
            draw_derived_int("MagicHitValue", confirmed_derived.MagicHitValue, preview_derived.MagicHitValue);
            draw_derived_int("MeleeCritValue", confirmed_derived.MeleeCritValue, preview_derived.MeleeCritValue);
            draw_derived_int("RangedCritValue", confirmed_derived.RangedCritValue, preview_derived.RangedCritValue);
            draw_derived_int("MagicCritValue", confirmed_derived.MagicCritValue, preview_derived.MagicCritValue);
            draw_derived_int("MeleeDmgMin", confirmed_derived.MeleeDmgMin, preview_derived.MeleeDmgMin);
            draw_derived_int("MeleeDmgMax", confirmed_derived.MeleeDmgMax, preview_derived.MeleeDmgMax);
            draw_derived_int("MagicDmgMin", confirmed_derived.MagicDmgMin, preview_derived.MagicDmgMin);
            draw_derived_int("MagicDmgMax", confirmed_derived.MagicDmgMax, preview_derived.MagicDmgMax);
            draw_derived_float("CritDamageMult", confirmed_derived.CritDamageMult, preview_derived.CritDamageMult);
            draw_derived_float("AttackSpeedMult", confirmed_derived.AttackSpeedMult, preview_derived.AttackSpeedMult);
            draw_derived_float("MovementSpeedMult", confirmed_derived.MovementSpeedMult, preview_derived.MovementSpeedMult);
            draw_derived_float("CooldownSpeedPct", confirmed_derived.CooldownSpeedPct, preview_derived.CooldownSpeedPct);
            draw_derived_float("SkillDamageBoostPct", confirmed_derived.SkillDamageBoostPct, preview_derived.SkillDamageBoostPct);
            draw_derived_float("BuffDurationPct", confirmed_derived.BuffDurationPct, preview_derived.BuffDurationPct);
            draw_derived_float("DebuffDurationPct", confirmed_derived.DebuffDurationPct, preview_derived.DebuffDurationPct);
            draw_derived_float("RangeBonusPct", confirmed_derived.RangeBonusPct, preview_derived.RangeBonusPct);
            draw_derived_int("BonusDamageFlat", confirmed_derived.BonusDamageFlat, preview_derived.BonusDamageFlat);
            draw_derived_int("CCChanceValue", confirmed_derived.CCChanceValue, preview_derived.CCChanceValue);
            draw_derived_int("CCResistanceValue", confirmed_derived.CCResistanceValue, preview_derived.CCResistanceValue);
            draw_derived_int("DamageReductionFlat", confirmed_derived.DamageReductionFlat, preview_derived.DamageReductionFlat);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Equipment")) {
            const ImVec2 equipOrigin = ImGui::GetCursorPos();
            const ImVec2 equipOriginS = ImGui::GetCursorScreenPos();

            ImVec2 silC = {
                equipOriginS.x + cw * 0.5f,
                equipOriginS.y + kColH * 0.40f
            };
            drawSilhouette(ImGui::GetWindowDrawList(), silC, 1.0f);

            for (int i = 0; i < kRows; ++i) {
                ImGui::SetCursorPos({equipOrigin.x, equipOrigin.y + i * (kSlotSz + kSlotGap)});
                drawEquipSlot(kLeftSlots[i], kSlotSz);
            }

            for (int i = 0; i < kRows; ++i) {
                ImGui::SetCursorPos({
                    equipOrigin.x + cw - kSlotSz,
                    equipOrigin.y + i * (kSlotSz + kSlotGap)
                });
                drawEquipSlot(kRightSlots[i], kSlotSz);
            }

            ImGui::SetCursorPos({equipOrigin.x, equipOrigin.y + kColH + 8.f});
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace rco::ui
