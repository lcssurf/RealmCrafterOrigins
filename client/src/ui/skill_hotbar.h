#pragma once

#include <cstdint>
#include <vector>

#include "../gameplay/skill_state.h"

struct ImVec2;

namespace rco::ui {

class SkillHotbar {
public:
    // Render dynamic skill hotbar from gameplay::PlayerSkillState().
    void Render(int screen_w, int screen_h);
    // Called when a fresh PSkillState snapshot is applied.
    void OnSkillStateUpdated(const gameplay::SkillState& state);

private:
    struct SlotCooldownState {
        uint32_t cooldown_total_ms_at_receipt = 0;
        int64_t  cooldown_set_at_ms = 0;
    };

    uint32_t ComputeLocalRemainingMs(int slot_index, int64_t now_ms);

    void RenderEmptySlot(int slot_index, float x0, float y0, float slot_w, float slot_h) const;
    void RenderAbilitySlot(int slot_index, const gameplay::SkillStateAbility& ability,
                           uint32_t cooldown_remaining_local_ms,
                           uint32_t cooldown_total_ms_at_receipt,
                           float x0, float y0, float slot_w, float slot_h) const;
    void DrawSkillTooltip(const gameplay::SkillStateAbility& ability, ImVec2 mouse_pos) const;

    std::vector<SlotCooldownState> slot_cooldowns_;
};

} // namespace rco::ui
