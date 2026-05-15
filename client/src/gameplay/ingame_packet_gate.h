#pragma once

#include <cstdint>

#include "net/codec.h"

namespace rco::gameplay {

// Returns true when packet type belongs to the in-game packet gate.
// Current behavior: packets are acknowledged and deferred to dedicated
// runtime systems as those domains are implemented.
bool HandleIngamePacketGate(uint16_t packet_type, rco::net::Reader& reader);

} // namespace rco::gameplay
