package protocol

// Packet type constants - values mirror RC's Packets.bb where applicable.
// RCO extensions start at 100.
const (
	PCreateAccount    uint16 = 1
	PVerifyAccount    uint16 = 2
	PFetchCharacter   uint16 = 3
	PCreateCharacter  uint16 = 4
	PDeleteCharacter  uint16 = 5
	PChangePassword   uint16 = 6
	PFetchActors      uint16 = 7
	PFetchItems       uint16 = 8
	PChangeArea       uint16 = 9
	PNewActor         uint16 = 11
	PStartGame        uint16 = 12
	PActorGone        uint16 = 13
	PStandardUpdate   uint16 = 14
	PInventoryUpdate  uint16 = 15
	PChatMessage      uint16 = 16
	PWeatherChange    uint16 = 17
	PAttackActor      uint16 = 18
	PActorDead        uint16 = 19
	PRightClick       uint16 = 20
	PDialog           uint16 = 21
	PStatUpdate       uint16 = 22
	PQuestLog         uint16 = 23
	PGoldChange       uint16 = 24
	PKnownSpells      uint16 = 26
	PCreateEmitter    uint16 = 28
	PSound            uint16 = 29
	PAnimateActor     uint16 = 30 // payload: rid(u32)+action_id(u8)
	PXPUpdate         uint16 = 32
	PMusic            uint16 = 34
	PProjectile       uint16 = 37
	PPartyUpdate      uint16 = 38
	PAppearanceUpdate uint16 = 39
	PFloatingNumber   uint16 = 48
	PRepositionActor  uint16 = 49
	PKickedPlayer     uint16 = 60

	// RCO extensions
	PLoginResult         uint16 = 100
	PCharListResult      uint16 = 101
	PCreateCharResult    uint16 = 102
	PDeleteCharResult    uint16 = 103
	PPing                uint16 = 104
	PPong                uint16 = 105
	PInventorySwap       uint16 = 106
	PUseItem             uint16 = 107
	PRespawnPlayer       uint16 = 108
	PPortalInfo          uint16 = 109
	PCastSpell           uint16 = 110
	PWorldItem           uint16 = 111
	PDialogChoice        uint16 = 112
	PPickupItem          uint16 = 113
	PRemoveWorldItem     uint16 = 114
	POpenShop            uint16 = 115
	PShopAction          uint16 = 116
	PAreaConfig          uint16 = 117 // S->C: per-area authoritative render config
	PPlayableDefs        uint16 = 118 // S->C: list of actor defs available for character creation
	PPlayerAction        uint16 = 119 // C->S: action(str)+state(u8)+axis_value(f32)
	PSetInputContext     uint16 = 120 // S->C: context(str)
	PInputBindings       uint16 = 121 // S->C: sends input bindings for the active preset
	PWorldObjects        uint16 = 122 // S->C: static world object instances for the current area
	PClientWorldReady    uint16 = 123 // C->S: client reached first playable frame after world enter
	PQuestAction         uint16 = 124 // C->S: quest action command
	PPartyAction         uint16 = 125 // C->S: party action command
	PCombatAction        uint16 = 126 // C->S: combat action command
	PSkillLoadoutAction  uint16 = 127 // C->S: skill loadout action command
	PCombatEvent         uint16 = 128 // S->C: combat feedback events
	PSkillState          uint16 = 129 // S->C: skill state snapshot
	PWeaponMasteryUpdate uint16 = 130 // S->C: weapon mastery progression delta
	PStatusEffectDelta   uint16 = 131 // S->C: status effect add/update/remove
	PKitPool             uint16 = 132 // S->C: active kit full ability pool snapshot
	PCastSkillSlot       uint16 = 133 // C->S: cast active loadout skill by hotbar slot
	PStatPointsUpdate    uint16 = 134 // S->C: unspent stat point pool update
	PPrimaryStatsUpdate  uint16 = 135 // S->C: primary stats + unspent snapshot
	PDistributeStatPoint uint16 = 136 // C->S: allocate primary stat points
	PRespec              uint16 = 137 // C->S: reset primary stats to base
	PFXCatalog           uint16 = 138 // S->C: full fx_templates catalog sent once on startgame
	PFullStats           uint16 = 139 // S->C: full stats sync (primary + vitals + 34 derived)
)

// EmitterType values for PCreateEmitter.
const (
	EmitterFire      uint8 = 0
	EmitterExplosion uint8 = 1
	EmitterHeal      uint8 = 2
	EmitterPortal    uint8 = 3
	EmitterBlood     uint8 = 4
	EmitterSmoke     uint8 = 5
)

// SoundID values for PSound.
const (
	SoundSwordHit    uint8 = 0
	SoundSpellFire   uint8 = 1
	SoundSpellHeal   uint8 = 2
	SoundSpellLight  uint8 = 3
	SoundNPCDeath    uint8 = 4
	SoundPlayerDeath uint8 = 5
	SoundLevelUp     uint8 = 6
	SoundPickupItem  uint8 = 7
	SoundPortal      uint8 = 8
	SoundBuyItem     uint8 = 9
	SoundSellItem    uint8 = 10
)

// MusicTrack values for PMusic (0 = stop).
const (
	MusicStop        uint8 = 0
	MusicStarterZone uint8 = 1
	MusicForest      uint8 = 2
	MusicCombat      uint8 = 3
)

// Result codes for server responses.
const (
	ResultOK            uint8 = 0
	ResultInvalidCreds  uint8 = 1
	ResultAccountExists uint8 = 2
	ResultBanned        uint8 = 3
	ResultAlreadyOnline uint8 = 4
	ResultServerFull    uint8 = 5
	ResultCharExists    uint8 = 6
	ResultInvalidName   uint8 = 7
)

// Quest action opcodes for PQuestAction.
const (
	QuestActionAccept  uint8 = 1
	QuestActionAbandon uint8 = 2
	QuestActionTurnIn  uint8 = 3
)

// Party action opcodes for PPartyAction.
const (
	PartyActionInvite       uint8 = 1
	PartyActionAccept       uint8 = 2
	PartyActionDecline      uint8 = 3
	PartyActionLeave        uint8 = 4
	PartyActionKick         uint8 = 5
	PartyActionTransferLead uint8 = 6
)

// Party notice/status codes carried by PPartyUpdate.
const (
	PartyNoticeNone                  uint8 = 0
	PartyNoticeInviteSent            uint8 = 1
	PartyNoticeInviteReceived        uint8 = 2
	PartyNoticeInviteDeclined        uint8 = 3
	PartyNoticeJoined                uint8 = 4
	PartyNoticeLeft                  uint8 = 5
	PartyNoticeKicked                uint8 = 6
	PartyNoticeLeaderTransferred     uint8 = 7
	PartyNoticeInviteCancelled       uint8 = 8
	PartyNoticeErrorTargetOffline    uint8 = 32
	PartyNoticeErrorCannotInviteSelf uint8 = 33
	PartyNoticeErrorTargetArea       uint8 = 34
	PartyNoticeErrorTargetDistance   uint8 = 35
	PartyNoticeErrorNoPendingInvite  uint8 = 36
	PartyNoticeErrorNotInParty       uint8 = 37
	PartyNoticeErrorNotLeader        uint8 = 38
	PartyNoticeErrorPartyFull        uint8 = 39
	PartyNoticeErrorAlreadyInParty   uint8 = 40
	PartyNoticeErrorInvitePending    uint8 = 41
	PartyNoticeErrorTargetNotInParty uint8 = 42
	PartyNoticeErrorUnsupported      uint8 = 43
	PartyNoticeErrorInvalidPayload   uint8 = 44
	PartyNoticeErrorPartyGone        uint8 = 45
)

// Combat action opcodes for PCombatAction.
const (
	CombatActionDodge      uint8 = 1
	CombatActionGuardStart uint8 = 2
	CombatActionGuardEnd   uint8 = 3
	CombatActionParryStart uint8 = 4
	CombatActionParryEnd   uint8 = 5
	CombatActionInterrupt  uint8 = 6
)

// Combat event codes carried by PCombatEvent.
const (
	CombatEventNone             uint8 = 0
	CombatEventActionRejected   uint8 = 1
	CombatEventDodgeStarted     uint8 = 2
	CombatEventGuardStarted     uint8 = 3
	CombatEventGuardEnded       uint8 = 4
	CombatEventParryStarted     uint8 = 5
	CombatEventParryEnded       uint8 = 6
	CombatEventInterruptSuccess uint8 = 7
	CombatEventHitDodged        uint8 = 8
	CombatEventHitGuarded       uint8 = 9
	CombatEventHitParried       uint8 = 10
	CombatEventSpecialWindup    uint8 = 11
	CombatEventSpecialParry     uint8 = 12
	CombatEventSpecialHit       uint8 = 13
	CombatEventCritHit          uint8 = 14
	CombatEventSpecialCritHit   uint8 = 15
)

// Skill loadout action opcodes for PSkillLoadoutAction.
const (
	SkillLoadoutActionSetSlot   uint8 = 1
	SkillLoadoutActionClearSlot uint8 = 2
	SkillLoadoutActionClearKit  uint8 = 3
)

// HeaderSize is the size in bytes of a packet header:
// 2 bytes (uint16 type) + 4 bytes (uint32 payload length).
const HeaderSize = 6
