#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/codec.h"

namespace rco { namespace gameplay {

struct KitPoolAbility {
    uint32_t    ability_id = 0;
    std::string ability_name;
    uint32_t    cooldown_ms = 0;
};

class ActiveKitPoolState {
public:
    bool has_kit() const { return kit_id_ != 0; }
    uint32_t kit_id() const { return kit_id_; }
    const std::string& kit_key() const { return kit_key_; }
    const std::string& kit_display_name() const { return kit_display_name_; }
    const std::vector<KitPoolAbility>& abilities() const { return abilities_; }

    // Parse packet body. Returns false on malformed payload.
    // On failure, internal state is unchanged.
    bool ApplyPacket(rco::net::Reader& r);
    void Clear();

private:
    uint32_t kit_id_ = 0;
    std::string kit_key_;
    std::string kit_display_name_;
    std::vector<KitPoolAbility> abilities_;
};

ActiveKitPoolState& MutableActiveKitPool();
const ActiveKitPoolState& ActiveKitPool();

}} // namespace rco::gameplay

