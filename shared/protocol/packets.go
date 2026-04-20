package protocol

// Packet type IDs — source of truth for client and server.
// Values mirror RC 1.26 Packets.bb where applicable.
// RCO-specific extensions start at 100.
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
	PNameChange       uint16 = 25
	PKnownSpellUpdate uint16 = 26
	PSpellUpdate      uint16 = 27
	PCreateEmitter    uint16 = 28
	PSound            uint16 = 29
	PAnimateActor     uint16 = 30
	PActionBarUpdate  uint16 = 31
	PXPUpdate         uint16 = 32
	PScreenFlash      uint16 = 33
	PMusic            uint16 = 34
	POpenTrading      uint16 = 35
	PActorEffect      uint16 = 36
	PProjectile       uint16 = 37
	PPartyUpdate      uint16 = 38
	PAppearanceUpdate uint16 = 39
	PCloseTrading     uint16 = 40
	PUpdateTrading    uint16 = 41
	PSelectScenery    uint16 = 42
	PItemScript       uint16 = 43
	PEatItem          uint16 = 44
	PItemHealth       uint16 = 45
	PJump             uint16 = 46
	PDismount         uint16 = 47
	PFloatingNumber   uint16 = 48
	PRepositionActor  uint16 = 49
	PSpeech           uint16 = 50
	PProgressBar      uint16 = 51
	PBubbleMessage    uint16 = 52
	PScriptInput      uint16 = 53
	PKickedPlayer     uint16 = 60

	// RCO extensions
	PLoginResult      uint16 = 100
	PCharListResult   uint16 = 101
	PCreateCharResult uint16 = 102
	PDeleteCharResult uint16 = 103
	PPing             uint16 = 104
	PPong             uint16 = 105
)

// Result codes returned in server response packets.
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

// HeaderSize is the size in bytes of every packet header:
// 2 bytes (uint16 packet type) + 4 bytes (uint32 payload length).
const HeaderSize = 6
