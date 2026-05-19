#pragma once

#include "../gameplay/skill_state.h"

struct ImVec2;

namespace rco::ui {

class SkillHotbar {
public:
    // Render dynamic skill hotbar from gameplay::PlayerSkillState().
    void Render(int screen_w, int screen_h);

private:
    void RenderEmptySlot(int slot_index, float x0, float y0, float slot_w, float slot_h) const;
    void RenderAbilitySlot(int slot_index, const gameplay::SkillStateAbility& ability,
                           float x0, float y0, float slot_w, float slot_h) const;
    void DrawSkillTooltip(const gameplay::SkillStateAbility& ability, ImVec2 mouse_pos) const;
};

} // namespace rco::ui
