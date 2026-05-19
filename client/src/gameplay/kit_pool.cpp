#include "kit_pool.h"

#include <cstdio>
#include <utility>

namespace rco { namespace gameplay {

static ActiveKitPoolState g_pool_state;

bool ActiveKitPoolState::ApplyPacket(rco::net::Reader& r) {
    uint8_t version = r.ReadU8();
    if (!r.OK()) {
        std::fprintf(stderr, "[KIT_POOL] underflow reading version\n");
        return false;
    }
    if (version != 1) {
        std::fprintf(stderr, "[KIT_POOL] unsupported version %u, ignoring\n",
                     static_cast<unsigned>(version));
        return false;
    }

    uint32_t kit_id = r.ReadU32();
    std::string kit_key = r.ReadString();
    std::string kit_display_name = r.ReadString();
    uint8_t ability_count = r.ReadU8();

    std::vector<KitPoolAbility> abilities;
    abilities.reserve(ability_count);
    for (uint8_t i = 0; i < ability_count; ++i) {
        KitPoolAbility a;
        a.ability_id = r.ReadU32();
        a.ability_name = r.ReadString();
        a.cooldown_ms = r.ReadU32();
        abilities.push_back(std::move(a));
    }

    if (!r.OK()) {
        std::fprintf(stderr, "[KIT_POOL] malformed packet, ignoring\n");
        return false;
    }

    kit_id_ = kit_id;
    kit_key_ = std::move(kit_key);
    kit_display_name_ = std::move(kit_display_name);
    abilities_ = std::move(abilities);
    return true;
}

void ActiveKitPoolState::Clear() {
    kit_id_ = 0;
    kit_key_.clear();
    kit_display_name_.clear();
    abilities_.clear();
}

ActiveKitPoolState& MutableActiveKitPool() {
    return g_pool_state;
}

const ActiveKitPoolState& ActiveKitPool() {
    return g_pool_state;
}

}} // namespace rco::gameplay

