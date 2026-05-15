#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace rco::ui {

struct SpellSlot {
    uint16_t    id          = 0;
    std::string name;
    uint8_t     spell_type  = 0;  // 0=damage 1=heal 2=buff 3=debuff
    uint8_t     aoe_type    = 0;  // 0=single 1=around_target 2=ground_target
    float       aoe_radius  = 0.f;
    float       range       = 0.f; // max cast range (0 = unlimited)
    uint16_t    mp_cost     = 0;
    uint32_t    cooldown_ms = 0;
    float       last_cast   = -99999.f;
};

class SpellBar {
public:
    std::vector<SpellSlot> slots;

    // Fired for single-target and AoE-around-target spells.
    std::function<void(uint16_t spell_id, uint32_t target_rid)> on_cast;

    // Fired for ground-targeted AoE spells (aoe_type == 2).
    std::function<void(uint16_t spell_id, float ground_x, float ground_z)> on_cast_ground;

    // Set when a ground-AoE spell is pending targeting (hotkey pressed, waiting for click).
    uint16_t pending_ground_spell = 0;

    void Clear();
    void AddSpell(uint16_t id, const std::string& name, uint8_t spell_type,
                  uint16_t mp_cost, uint32_t cooldown_ms,
                  uint8_t aoe_type = 0, float aoe_radius = 0.f, float range = 0.f);

    // Render the spell bar.
    // target_dist: distance to combat_target in world units (pass 0 if no target).
    void Render(int screen_w, int screen_h, uint32_t combat_target,
                float now, bool player_dead, int32_t player_mp,
                float target_dist = 0.f);

    // Updated each Render() call — read by main to draw world-space circles.
    float   hovered_range      = 0.f;
    float   hovered_aoe_radius = 0.f;
    uint8_t hovered_aoe_type   = 0;
};

} // namespace rco::ui
