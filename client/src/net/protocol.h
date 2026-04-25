#pragma once

#include <cstdint>

namespace rco::net {

// ---------------------------------------------------------------------------
// Packet type IDs — must match server exactly
// ---------------------------------------------------------------------------
constexpr uint16_t kPCreateAccount    = 1;
constexpr uint16_t kPVerifyAccount    = 2;
constexpr uint16_t kPFetchCharacter   = 3;
constexpr uint16_t kPCreateCharacter  = 4;
constexpr uint16_t kPDeleteCharacter  = 5;
constexpr uint16_t kPChangeArea       = 9;
constexpr uint16_t kPStartGame        = 12;
constexpr uint16_t kPNewActor         = 11;
constexpr uint16_t kPActorGone        = 13;
constexpr uint16_t kPStandardUpdate   = 14;
constexpr uint16_t kPInventoryUpdate  = 15;
constexpr uint16_t kPChatMessage      = 16;
constexpr uint16_t kPAttackActor      = 18;
constexpr uint16_t kPActorDead        = 19;
constexpr uint16_t kPRightClick       = 20;
constexpr uint16_t kPDialog           = 21;
constexpr uint16_t kPStatUpdate       = 22;
constexpr uint16_t kPXPUpdate         = 32;
constexpr uint16_t kPFloatingNumber   = 48;
constexpr uint16_t kPRepositionActor  = 49;
constexpr uint16_t kPAnimateActor     = 30;  // payload: rid(u32)+state(u8) — 0=Idle 1=Walk 2=Attack 3=Death
constexpr uint16_t kPKickedPlayer     = 60;
constexpr uint16_t kPLoginResult      = 100;
constexpr uint16_t kPCharListResult   = 101;
constexpr uint16_t kPCreateCharResult = 102;
constexpr uint16_t kPDeleteCharResult = 103;
constexpr uint16_t kPPing             = 104;
constexpr uint16_t kPPong             = 105;
constexpr uint16_t kPInventorySwap    = 106;
constexpr uint16_t kPUseItem          = 107;
constexpr uint16_t kPRespawnPlayer    = 108;
constexpr uint16_t kPPortalInfo       = 109;
constexpr uint16_t kPGoldChange       = 24;
constexpr uint16_t kPKnownSpells      = 26;
constexpr uint16_t kPCreateEmitter    = 28;
constexpr uint16_t kPSound            = 29;
constexpr uint16_t kPMusic            = 34;
constexpr uint16_t kPCastSpell        = 110;
constexpr uint16_t kPWorldItem        = 111;
constexpr uint16_t kPDialogChoice     = 112;
constexpr uint16_t kPPickupItem       = 113;
constexpr uint16_t kPRemoveWorldItem  = 114;
constexpr uint16_t kPOpenShop         = 115;
constexpr uint16_t kPShopAction       = 116;
constexpr uint16_t kPPlayableDefs     = 118; // S→C: list of actor defs available for character creation

// ---------------------------------------------------------------------------
// Result codes
// ---------------------------------------------------------------------------
constexpr uint8_t kResultOK             = 0;
constexpr uint8_t kResultInvalidCreds   = 1;
constexpr uint8_t kResultAccountExists  = 2;
constexpr uint8_t kResultBanned         = 3;
constexpr uint8_t kResultAlreadyOnline  = 4;
constexpr uint8_t kResultCharExists     = 6;

// ---------------------------------------------------------------------------
// Frame layout: [uint16 type LE][uint32 payloadLen LE][payload bytes]
// ---------------------------------------------------------------------------
constexpr size_t kHeaderSize = 6;  // 2 (type) + 4 (length)

struct PacketHeader {
    uint16_t type;
    uint32_t length;
};

} // namespace rco::net
