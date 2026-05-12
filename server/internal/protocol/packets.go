package protocol

// Packet type constants â€” values mirror RC's Packets.bb where applicable.
// RCO extensions start at 100.
const (
	PCreateAccount   uint16 = 1
	PVerifyAccount   uint16 = 2
	PFetchCharacter  uint16 = 3
	PCreateCharacter uint16 = 4
	PDeleteCharacter uint16 = 5
	PChangePassword  uint16 = 6
	PFetchActors     uint16 = 7
	PFetchItems      uint16 = 8
	PChangeArea      uint16 = 9
	PNewActor        uint16 = 11
	PStartGame       uint16 = 12
	PActorGone       uint16 = 13
	PStandardUpdate  uint16 = 14
	PInventoryUpdate uint16 = 15
	PChatMessage     uint16 = 16
	PAttackActor     uint16 = 18
	PActorDead       uint16 = 19
	PRightClick      uint16 = 20
	PDialog          uint16 = 21
	PStatUpdate      uint16 = 22
	PFloatingNumber  uint16 = 48
	PGoldChange      uint16 = 24
	PXPUpdate        uint16 = 32
	PKnownSpells     uint16 = 26
	PCreateEmitter   uint16 = 28
	PSound           uint16 = 29
	PMusic           uint16 = 34
	PRepositionActor uint16 = 49
	PAnimateActor    uint16 = 30 // payload: rid(u32)+action_id(u8)
	PKickedPlayer    uint16 = 60

	// RCO extensions
	PLoginResult      uint16 = 100
	PCharListResult   uint16 = 101
	PCreateCharResult uint16 = 102
	PDeleteCharResult uint16 = 103
	PPing             uint16 = 104
	PPong             uint16 = 105
	PInventorySwap    uint16 = 106
	PUseItem          uint16 = 107
	PRespawnPlayer    uint16 = 108
	PPortalInfo       uint16 = 109
	PCastSpell        uint16 = 110
	PWorldItem        uint16 = 111
	PDialogChoice     uint16 = 112
	PPickupItem       uint16 = 113
	PRemoveWorldItem  uint16 = 114
	POpenShop         uint16 = 115
	PShopAction       uint16 = 116
	PAreaConfig       uint16 = 117 // Sâ†’C: per-area authoritative render config (skybox/light/fog/look/color/terrain-tuning)
	PPlayableDefs     uint16 = 118 // Sâ†’C: list of actor defs available for character creation
	PPlayerAction     uint16 = 119 // Câ†’S: action(str)+state(u8)+axis_value(f32)
	PSetInputContext  uint16 = 120 // Sâ†’C: context(str)
	PInputBindings    uint16 = 121 // Sâ†’C: sends input bindings for the active preset
	PWorldObjects     uint16 = 122 // S->C: static world object instances for the current area
	PClientWorldReady uint16 = 123 // C->S: client reached first playable frame after world enter
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

// HeaderSize is the size in bytes of a packet header:
// 2 bytes (uint16 type) + 4 bytes (uint32 payload length).
const HeaderSize = 6
