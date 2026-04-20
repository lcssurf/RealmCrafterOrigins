package protocol

// Packet type constants — values mirror RC's Packets.bb where applicable.
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
	PXPUpdate         uint16 = 32
	PKnownSpells      uint16 = 26
	PRepositionActor uint16 = 49
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
	PDialogChoice     uint16 = 112
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
