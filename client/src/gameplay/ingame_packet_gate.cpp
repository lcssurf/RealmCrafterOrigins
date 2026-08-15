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
            return true;
        // kPStatusEffectDelta REMOVED from this swallow-list (Fase 1,
        // Buffs/Debuffs/CC — see docs/TECH_DEBT.md): this gate returning
        // true means "recognized, but do nothing" — the packet was
        // reserved here without ever being parsed anywhere, making it dead
        // wire (confirmed in the investigation before this round). Real
        // parsing now happens in main.cpp's packet switch; letting it fall
        // through to `default: return false` is what lets that switch see
        // it at all.
        default:
            return false;
    }
}

} // namespace rco::gameplay
