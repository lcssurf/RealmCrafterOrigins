#include "skill_hotbar.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <imgui.h>

namespace rco::ui {

void SkillHotbar::Render(int screen_w, int screen_h) {
    const auto& state = gameplay::PlayerSkillState();

    int max_slot = -1;
    for (const auto& ab : state.abilities()) {
        max_slot = std::max(max_slot, static_cast<int>(ab.slot_index));
    }

    // Current packet does not carry explicit "hotbar slots granted" yet.
    // Heuristic for this phase:
    // - active kit: show at least 4 slots, or grow to max received slot index.
    // - no active kit: show empty placeholders (4 slots).
    int slot_count = 4;
    if (state.has_kit()) {
        slot_count = std::max(4, max_slot + 1);
    }
    if (slot_count < 1) slot_count = 1;

    constexpr float kSlotW = 60.f;
    constexpr float kSlotH = 60.f;
    constexpr float kGap = 4.f;
    const float total_w = slot_count * kSlotW + (slot_count - 1) * kGap;
    const float ox = (static_cast<float>(screen_w) - total_w) * 0.5f;
    const float oy = static_cast<float>(screen_h) - kSlotH - 96.f; // above legacy SpellBar

    ImGui::SetNextWindowPos({ox - 8.f, oy - 8.f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({total_w + 16.f, kSlotH + 16.f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##skill_hotbar", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs);

    std::vector<const gameplay::SkillStateAbility*> by_slot(static_cast<std::size_t>(slot_count), nullptr);
    for (const auto& ab : state.abilities()) {
        const int idx = static_cast<int>(ab.slot_index);
        if (idx >= 0 && idx < slot_count) {
            by_slot[static_cast<std::size_t>(idx)] = &ab;
        }
    }

    struct SlotRect {
        ImVec2 top_left;
        ImVec2 size;
        const gameplay::SkillStateAbility* ability = nullptr;
    };
    std::vector<SlotRect> slot_rects;
    slot_rects.reserve(static_cast<std::size_t>(slot_count));

    for (int i = 0; i < slot_count; ++i) {
        const float x0 = ox + i * (kSlotW + kGap);
        const float y0 = oy;
        const auto* ability = by_slot[static_cast<std::size_t>(i)];
        if (by_slot[static_cast<std::size_t>(i)] != nullptr) {
            RenderAbilitySlot(i, *ability, x0, y0, kSlotW, kSlotH);
        } else {
            RenderEmptySlot(i, x0, y0, kSlotW, kSlotH);
        }
        slot_rects.push_back(SlotRect{
            ImVec2{x0, y0},
            ImVec2{kSlotW, kSlotH},
            ability,
        });
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    for (const auto& rect : slot_rects) {
        if (rect.ability == nullptr) continue;

        const bool inside =
            mouse.x >= rect.top_left.x &&
            mouse.x <= rect.top_left.x + rect.size.x &&
            mouse.y >= rect.top_left.y &&
            mouse.y <= rect.top_left.y + rect.size.y;
        if (!inside) continue;

        DrawSkillTooltip(*rect.ability, mouse);
        break;
    }

    ImGui::End();
}

void SkillHotbar::RenderEmptySlot(int slot_index, float x0, float y0, float slot_w, float slot_h) const {
    auto* dl = ImGui::GetWindowDrawList();
    const float x1 = x0 + slot_w;
    const float y1 = y0 + slot_h;
    const ImVec2 p0{x0, y0};
    const ImVec2 p1{x1, y1};

    dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 24, 200), 5.f);
    dl->AddRect(p0, p1, IM_COL32(70, 70, 85, 180), 5.f, 0, 1.5f);

    char hk[4];
    std::snprintf(hk, sizeof(hk), "%d", slot_index + 1);
    dl->AddText({x0 + 4.f, y1 - 15.f}, IM_COL32(140, 140, 150, 180), hk);
}

void SkillHotbar::RenderAbilitySlot(int slot_index, const gameplay::SkillStateAbility& ability,
                                    float x0, float y0, float slot_w, float slot_h) const {
    auto* dl = ImGui::GetWindowDrawList();
    const float x1 = x0 + slot_w;
    const float y1 = y0 + slot_h;
    const ImVec2 p0{x0, y0};
    const ImVec2 p1{x1, y1};

    dl->AddRectFilled(p0, p1, IM_COL32(20, 28, 52, 220), 5.f);
    dl->AddRect(p0, p1, IM_COL32(100, 140, 255, 210), 5.f, 0, 1.8f);

    char abbr[12];
    std::snprintf(abbr, sizeof(abbr), "%.10s", ability.ability_name.c_str());
    ImVec2 ts = ImGui::CalcTextSize(abbr);
    dl->AddText({x0 + (slot_w - ts.x) * 0.5f, y0 + 6.f}, IM_COL32(240, 240, 240, 220), abbr);

    if (ability.cooldown_remaining_ms > 0 && ability.cooldown_ms > 0) {
        const float ratio = std::clamp(
            static_cast<float>(ability.cooldown_remaining_ms) / static_cast<float>(ability.cooldown_ms),
            0.f, 1.f);
        dl->AddRectFilled(p0, {x1, y0 + slot_h * ratio}, IM_COL32(0, 0, 0, 130), 5.f);
    }

    char hk[4];
    std::snprintf(hk, sizeof(hk), "%d", slot_index + 1);
    dl->AddText({x0 + 4.f, y1 - 15.f}, IM_COL32(180, 180, 190, 210), hk);
}

void SkillHotbar::DrawSkillTooltip(const gameplay::SkillStateAbility& ability, ImVec2 mouse_pos) const {
    auto* dl = ImGui::GetForegroundDrawList();

    char line1[128];
    char line2[64];
    char line3[96];
    if (ability.mastery_level > 0) {
        std::snprintf(line1, sizeof(line1), "%s (Lv %u)",
                      ability.ability_name.c_str(),
                      static_cast<unsigned>(ability.mastery_level));
    } else {
        std::snprintf(line1, sizeof(line1), "%s", ability.ability_name.c_str());
    }
    std::snprintf(line2, sizeof(line2), "Cooldown: %u ms", ability.cooldown_ms);

    const ImVec2 size1 = ImGui::CalcTextSize(line1);
    const ImVec2 size2 = ImGui::CalcTextSize(line2);
    bool show_line3 = false;
    ImU32 line3_color = IM_COL32(150, 200, 150, 255);
    if (ability.mastery_level > 0 && ability.mastery_max_level > 0) {
        if (ability.mastery_level >= ability.mastery_max_level) {
            std::snprintf(line3, sizeof(line3), "MAX LEVEL");
            show_line3 = true;
            line3_color = IM_COL32(255, 215, 0, 255);
        } else {
            std::snprintf(line3, sizeof(line3), "XP: %u / %u",
                          ability.mastery_xp, ability.mastery_xp_for_next);
            show_line3 = true;
        }
    }
    const ImVec2 size3 = show_line3 ? ImGui::CalcTextSize(line3) : ImVec2(0.f, 0.f);

    constexpr float kPadding = 6.0f;
    float tooltip_w = std::max(size1.x, size2.x);
    if (show_line3) {
        tooltip_w = std::max(tooltip_w, size3.x);
    }
    tooltip_w += kPadding * 2.0f;

    float tooltip_h = size1.y + size2.y + kPadding * 3.0f;
    if (show_line3) {
        tooltip_h += size3.y + kPadding;
    }

    ImVec2 pos(mouse_pos.x + 14.0f, mouse_pos.y - tooltip_h - 8.0f);
    const ImGuiIO& io = ImGui::GetIO();
    if (pos.y < 0.0f) pos.y = mouse_pos.y + 18.0f;
    if (pos.x + tooltip_w > io.DisplaySize.x) pos.x = io.DisplaySize.x - tooltip_w - 4.0f;
    if (pos.x < 0.0f) pos.x = 0.0f;

    const ImVec2 p1(pos.x + tooltip_w, pos.y + tooltip_h);
    dl->AddRectFilled(pos, p1, IM_COL32(20, 20, 25, 230), 4.0f);
    dl->AddRect(pos, p1, IM_COL32(80, 80, 90, 255), 4.0f);
    dl->AddText(ImVec2(pos.x + kPadding, pos.y + kPadding),
                IM_COL32(255, 255, 255, 255), line1);
    dl->AddText(ImVec2(pos.x + kPadding, pos.y + kPadding * 2.0f + size1.y),
                IM_COL32(180, 180, 180, 255), line2);
    if (show_line3) {
        dl->AddText(ImVec2(pos.x + kPadding, pos.y + kPadding * 3.0f + size1.y + size2.y),
                    line3_color, line3);
    }
}

} // namespace rco::ui
