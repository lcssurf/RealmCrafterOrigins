// Package world - combat.go
//
// Shared combat constants and lightweight types used across focused combat files.
package world

const (
	CombatDelay            = 800 // ms between attacks
	MeleeRange             = 2.0 // default melee attack range (world units)
	pStandardUpdate uint16 = 14
	guardDamagePct  int32  = 40 // guarded hits deal 40% damage
	guardHitSPCost  int32  = 8  // SP consumed when absorbing a guarded hit
)

// Packet type constants mirrored from protocol (avoids circular import).
const (
	pAttackActor     uint16 = 18
	pActorDead       uint16 = 19
	pStatUpdate      uint16 = 22
	pNewActor        uint16 = 11
	pActorGone       uint16 = 13
	pFloatingNum     uint16 = 48
	pWorldItem       uint16 = 111
	pRemoveWorldItem uint16 = 114
	pAnimateActor    uint16 = 30
	pCombatEvent     uint16 = 128
)

const (
	npcSpecialWindupMsLegacy   int64 = 1300
	npcSpecialCooldownMsLegacy int64 = 6500
	npcSpecialChancePctLegacy  int   = 30
	npcSpecialParryExactMs     int64 = 220
	npcSpecialGlobalGCDMs      int64 = 450
	npcSpecialMinDamage        int32 = 35
)

// AttackResult describes how an attack attempt resolved.
type AttackResult uint8

const (
	AttackResultNormal AttackResult = iota
	AttackResultMiss
	AttackResultDodged
	AttackResultGuarded
	AttackResultParried
)

type npcSpecialIntent struct {
	Ability AbilityTemplate
}

type loadoutEvalContext struct {
	distance    float32
	npcHPPct    float32
	targetHPPct float32
	npcSPPct    float32
	targetSPPct float32
	npcMPPct    float32
	targetMPPct float32
	phaseTag    string
	phaseIndex  float64
}
