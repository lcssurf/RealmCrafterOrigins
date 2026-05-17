#include "ingame_packet_gate.h"

#include "net/protocol.h"

namespace rco::gameplay {

bool HandleIngamePacketGate(uint16_t packet_type, rco::net::Reader& reader) {
    (void)reader;

    switch (packet_type) {
        case rco::net::kPWeatherChange:
        case rco::net::kPProjectile:
        case rco::net::kPAppearanceUpdate:
        case rco::net::kPWeaponMasteryUpdate:
        case rco::net::kPStatusEffectDelta:
            return true;
        default:
            return false;
    }
}

} // namespace rco::gameplay
