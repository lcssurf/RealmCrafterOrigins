#pragma once

#include <cstddef>
#include <cstdint>

namespace rco::net {

// ---------------------------------------------------------------------------
// Packet type IDs - must match server exactly
// ---------------------------------------------------------------------------
constexpr uint16_t kPCreateAccount     = 1;
constexpr uint16_t kPVerifyAccount     = 2;
constexpr uint16_t kPFetchCharacter    = 3;
constexpr uint16_t kPCreateCharacter   = 4;
constexpr uint16_t kPDeleteCharacter   = 5;
constexpr uint16_t kPChangeArea        = 9;
constexpr uint16_t kPNewActor          = 11;
constexpr uint16_t kPStartGame         = 12;
constexpr uint16_t kPActorGone         = 13;
constexpr uint16_t kPStandardUpdate    = 14;
constexpr uint16_t kPInventoryUpdate   = 15;
constexpr uint16_t kPChatMessage       = 16;
constexpr uint16_t kPWeatherChange     = 17;
constexpr uint16_t kPAttackActor       = 18;
constexpr uint16_t kPActorDead         = 19;
constexpr uint16_t kPRightClick        = 20;
constexpr uint16_t kPDialog            = 21;
constexpr uint16_t kPStatUpdate        = 22;
constexpr uint16_t kPQuestLog          = 23;
constexpr uint16_t kPGoldChange        = 24;
constexpr uint16_t kPKnownSpells       = 26;
constexpr uint16_t kPCreateEmitter     = 28;
constexpr uint16_t kPSound             = 29;
constexpr uint16_t kPAnimateActor      = 30;  // payload: rid(u32)+action_id(u8)
constexpr uint16_t kPXPUpdate          = 32;
constexpr uint16_t kPMusic             = 34;
constexpr uint16_t kPProjectile        = 37;
constexpr uint16_t kPPartyUpdate       = 38;
constexpr uint16_t kPAppearanceUpdate  = 39;
constexpr uint16_t kPFloatingNumber    = 48;
constexpr uint16_t kPRepositionActor   = 49;
constexpr uint16_t kPKickedPlayer      = 60;

constexpr uint16_t kPLoginResult       = 100;
constexpr uint16_t kPCharListResult    = 101;
constexpr uint16_t kPCreateCharResult  = 102;
constexpr uint16_t kPDeleteCharResult  = 103;
constexpr uint16_t kPPing              = 104;
constexpr uint16_t kPPong              = 105;
constexpr uint16_t kPInventorySwap     = 106;
constexpr uint16_t kPUseItem           = 107;
constexpr uint16_t kPRespawnPlayer     = 108;
constexpr uint16_t kPPortalInfo        = 109;
constexpr uint16_t kPCastSpell         = 110;
constexpr uint16_t kPWorldItem         = 111;
constexpr uint16_t kPDialogChoice      = 112;
constexpr uint16_t kPPickupItem        = 113;
constexpr uint16_t kPRemoveWorldItem   = 114;
constexpr uint16_t kPOpenShop          = 115;
constexpr uint16_t kPShopAction        = 116;
constexpr uint16_t kPAreaConfig        = 117; // S->C: per-area authoritative render config
constexpr uint16_t kPPlayableDefs      = 118; // S->C: list of actor defs available for character creation
constexpr uint16_t kPPlayerAction      = 119; // C->S: action(str)+state(u8)+axis(f32)
constexpr uint16_t kPSetInputContext   = 120; // S->C: context(str)
constexpr uint16_t kPInputBindings     = 121; // S->C: input bindings for active preset
constexpr uint16_t kPWorldObjects      = 122; // S->C: static world object instances for current area
constexpr uint16_t kPClientWorldReady  = 123; // C->S: client reached first playable frame after world enter
constexpr uint16_t kPQuestAction       = 124; // C->S: quest action command
constexpr uint16_t kPPartyAction       = 125; // C->S: party action command
constexpr uint16_t kPCombatAction      = 126; // C->S: combat action command
constexpr uint16_t kPSkillLoadoutAction = 127; // C->S: skill loadout command
constexpr uint16_t kPCombatEvent       = 128; // S->C: combat feedback events
constexpr uint16_t kPSkillState        = 129; // S->C: skill state snapshot
constexpr uint16_t kPWeaponMasteryUpdate = 130; // S->C: weapon mastery progression delta
constexpr uint16_t kPStatusEffectDelta = 131; // S->C: status effect add/update/remove
constexpr uint16_t kPKitPool           = 132; // S->C: active kit full ability pool snapshot
constexpr uint16_t kPCastSkillSlot     = 133; // C->S: cast active loadout skill by hotbar slot
constexpr uint16_t kPStatPointsUpdate  = 134; // S->C: unspent stat point pool update
constexpr uint16_t kPPrimaryStatsUpdate = 135; // S->C: primary stats + unspent snapshot
constexpr uint16_t kPDistributeStatPoint = 136; // C->S: allocate primary stat points
constexpr uint16_t kPRespec            = 137; // C->S: reset primary stats to base
constexpr uint16_t kPFXCatalog         = 138;

// ---------------------------------------------------------------------------
// Result codes
// ---------------------------------------------------------------------------
constexpr uint8_t kResultOK            = 0;
constexpr uint8_t kResultInvalidCreds  = 1;
constexpr uint8_t kResultAccountExists = 2;
constexpr uint8_t kResultBanned        = 3;
constexpr uint8_t kResultAlreadyOnline = 4;
constexpr uint8_t kResultCharExists    = 6;

// ---------------------------------------------------------------------------
// In-game action opcodes (client -> server)
// ---------------------------------------------------------------------------
constexpr uint8_t kQuestActionAccept  = 1;
constexpr uint8_t kQuestActionAbandon = 2;
constexpr uint8_t kQuestActionTurnIn  = 3;

constexpr uint8_t kPartyActionInvite       = 1;
constexpr uint8_t kPartyActionAccept       = 2;
constexpr uint8_t kPartyActionDecline      = 3;
constexpr uint8_t kPartyActionLeave        = 4;
constexpr uint8_t kPartyActionKick         = 5;
constexpr uint8_t kPartyActionTransferLead = 6;

// Party notice/status codes carried by PPartyUpdate.
constexpr uint8_t kPartyNoticeNone                  = 0;
constexpr uint8_t kPartyNoticeInviteSent            = 1;
constexpr uint8_t kPartyNoticeInviteReceived        = 2;
constexpr uint8_t kPartyNoticeInviteDeclined        = 3;
constexpr uint8_t kPartyNoticeJoined                = 4;
constexpr uint8_t kPartyNoticeLeft                  = 5;
constexpr uint8_t kPartyNoticeKicked                = 6;
constexpr uint8_t kPartyNoticeLeaderTransferred     = 7;
constexpr uint8_t kPartyNoticeInviteCancelled       = 8;
constexpr uint8_t kPartyNoticeErrorTargetOffline    = 32;
constexpr uint8_t kPartyNoticeErrorCannotInviteSelf = 33;
constexpr uint8_t kPartyNoticeErrorTargetArea       = 34;
constexpr uint8_t kPartyNoticeErrorTargetDistance   = 35;
constexpr uint8_t kPartyNoticeErrorNoPendingInvite  = 36;
constexpr uint8_t kPartyNoticeErrorNotInParty       = 37;
constexpr uint8_t kPartyNoticeErrorNotLeader        = 38;
constexpr uint8_t kPartyNoticeErrorPartyFull        = 39;
constexpr uint8_t kPartyNoticeErrorAlreadyInParty   = 40;
constexpr uint8_t kPartyNoticeErrorInvitePending    = 41;
constexpr uint8_t kPartyNoticeErrorTargetNotInParty = 42;
constexpr uint8_t kPartyNoticeErrorUnsupported      = 43;
constexpr uint8_t kPartyNoticeErrorInvalidPayload   = 44;
constexpr uint8_t kPartyNoticeErrorPartyGone        = 45;

constexpr uint8_t kCombatActionDodge      = 1;
constexpr uint8_t kCombatActionGuardStart = 2;
constexpr uint8_t kCombatActionGuardEnd   = 3;
constexpr uint8_t kCombatActionParryStart = 4;
constexpr uint8_t kCombatActionParryEnd   = 5;
constexpr uint8_t kCombatActionInterrupt  = 6;

// Combat event codes carried by PCombatEvent.
constexpr uint8_t kCombatEventNone             = 0;
constexpr uint8_t kCombatEventActionRejected   = 1;
constexpr uint8_t kCombatEventDodgeStarted     = 2;
constexpr uint8_t kCombatEventGuardStarted     = 3;
constexpr uint8_t kCombatEventGuardEnded       = 4;
constexpr uint8_t kCombatEventParryStarted     = 5;
constexpr uint8_t kCombatEventParryEnded       = 6;
constexpr uint8_t kCombatEventInterruptSuccess = 7;
constexpr uint8_t kCombatEventHitDodged        = 8;
constexpr uint8_t kCombatEventHitGuarded       = 9;
constexpr uint8_t kCombatEventHitParried       = 10;
constexpr uint8_t kCombatEventSpecialWindup    = 11;
constexpr uint8_t kCombatEventSpecialParry     = 12;
constexpr uint8_t kCombatEventSpecialHit       = 13;
constexpr uint8_t kCombatEventCritHit          = 14;
constexpr uint8_t kCombatEventSpecialCritHit   = 15;

constexpr uint8_t kSkillLoadoutActionSetSlot   = 1;
constexpr uint8_t kSkillLoadoutActionClearSlot = 2;
constexpr uint8_t kSkillLoadoutActionClearKit  = 3;

// Legacy aliases kept for compatibility with older call-sites.
constexpr uint8_t kSkillLoadoutActionEquip      = kSkillLoadoutActionSetSlot;
constexpr uint8_t kSkillLoadoutActionUnequip    = kSkillLoadoutActionClearSlot;
constexpr uint8_t kSkillLoadoutActionSwapPreset = kSkillLoadoutActionClearKit;

// ---------------------------------------------------------------------------
// Frame layout: [uint16 type LE][uint32 payloadLen LE][payload bytes]
// ---------------------------------------------------------------------------
constexpr size_t kHeaderSize = 6; // 2 (type) + 4 (length)

struct PacketHeader {
    uint16_t type;
    uint32_t length;
};

} // namespace rco::net
