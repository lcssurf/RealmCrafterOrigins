#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/codec.h"

namespace rco { namespace gameplay {

struct SkillStateAbility {
    uint8_t     slot_index = 0;
    uint32_t    ability_id = 0;
    std::string ability_name;
    uint32_t    cooldown_ms = 0;
    uint32_t    cooldown_remaining_ms = 0;
    uint8_t     mastery_level = 0;
    uint32_t    mastery_xp = 0;
    uint32_t    mastery_xp_current_level_thr = 0;
    uint32_t    mastery_xp_for_next = 0;
    uint8_t     mastery_max_level = 0;
    std::string description;
};

class SkillState {
public:
    bool has_kit() const { return has_kit_; }
    uint32_t kit_id() const { return kit_id_; }
    const std::string& kit_key() const { return kit_key_; }
    const std::string& kit_display_name() const { return kit_display_name_; }
    const std::vector<SkillStateAbility>& abilities() const { return abilities_; }
    void Clear();

    // Parse packet body. Returns false on malformed payload.
    // On failure, internal state is unchanged.
    bool ApplyPacket(rco::net::Reader& r);

private:
    bool        has_kit_ = false;
    uint32_t    kit_id_ = 0;
    std::string kit_key_;
    std::string kit_display_name_;
    std::vector<SkillStateAbility> abilities_;
};

SkillState& MutablePlayerSkillState();
const SkillState& PlayerSkillState();

}} // namespace rco::gameplay
