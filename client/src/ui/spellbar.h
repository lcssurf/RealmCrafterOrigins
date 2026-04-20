#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace rco::ui {

struct SpellSlot {
    uint16_t    id          = 0;
    std::string name;
    uint8_t     spell_type  = 0;  // 0=damage, 1=heal, 2=buff
    uint16_t    ep_cost     = 0;
    uint32_t    cooldown_ms = 0;
    float       last_cast   = -99999.f;
};

class SpellBar {
public:
    std::vector<SpellSlot> slots;

    // Called when the player activates a spell.
    // spell_id: the spell to cast. target_rid: 0 for self-target.
    std::function<void(uint16_t spell_id, uint32_t target_rid)> on_cast;

    void Clear();
    void AddSpell(uint16_t id, const std::string& name, uint8_t spell_type,
                  uint16_t ep_cost, uint32_t cooldown_ms);

    // Render the spell bar at the bottom-center of the screen.
    // combat_target: currently selected target RID (0 = none).
    void Render(int screen_w, int screen_h, uint32_t combat_target,
                float now, bool player_dead, int32_t player_ep);
};

} // namespace rco::ui
