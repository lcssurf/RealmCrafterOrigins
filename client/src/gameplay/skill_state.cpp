#include "skill_state.h"

#include <cstdio>
#include <utility>

namespace rco { namespace gameplay {

static SkillState g_state;

bool SkillState::ApplyPacket(rco::net::Reader& r) {
    uint8_t version = r.ReadU8();
    if (!r.OK()) {
        std::fprintf(stderr, "[SKILL_STATE] underflow reading version\n");
        return false;
    }
    if (version != 1 && version != 2 && version != 3 && version != 4) {
        std::fprintf(stderr, "[SKILL_STATE] unsupported version %u, ignoring\n",
                     static_cast<unsigned>(version));
        return false;
    }

    const bool has_kit = (r.ReadU8() != 0);
    const uint32_t kit_id = r.ReadU32();
    std::string kit_key = r.ReadString();
    std::string kit_display_name = r.ReadString();
    uint8_t ability_count = r.ReadU8();

    std::vector<SkillStateAbility> abilities;
    abilities.reserve(ability_count);
    for (uint8_t i = 0; i < ability_count; ++i) {
        SkillStateAbility a;
        a.slot_index = r.ReadU8();
        a.ability_id = r.ReadU32();
        a.ability_name = r.ReadString();
        a.cooldown_ms = r.ReadU32();
        a.cooldown_remaining_ms = r.ReadU32();
        if (version >= 4) {
            a.mastery_level = r.ReadU8();
            a.mastery_xp = r.ReadU32();
            a.mastery_xp_current_level_thr = r.ReadU32();
            a.mastery_xp_for_next = r.ReadU32();
            a.mastery_max_level = r.ReadU8();
        } else if (version >= 2) {
            a.mastery_level = r.ReadU8();
            a.mastery_xp = r.ReadU32();
            a.mastery_xp_for_next = r.ReadU32();
            a.mastery_max_level = r.ReadU8();
        }
        if (version >= 3) {
            a.description = r.ReadString();
        }
        abilities.push_back(std::move(a));
    }

    if (!r.OK()) {
        std::fprintf(stderr, "[SKILL_STATE] malformed packet, ignoring\n");
        return false;
    }

    has_kit_ = has_kit;
    kit_id_ = kit_id;
    kit_key_ = std::move(kit_key);
    kit_display_name_ = std::move(kit_display_name);
    abilities_ = std::move(abilities);
    return true;
}

void SkillState::Clear() {
    has_kit_ = false;
    kit_id_ = 0;
    kit_key_.clear();
    kit_display_name_.clear();
    abilities_.clear();
}

SkillState& MutablePlayerSkillState() {
    return g_state;
}

const SkillState& PlayerSkillState() {
    return g_state;
}

}} // namespace rco::gameplay
