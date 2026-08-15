package world

import "strings"

// CombatDimension identifies which combat stat trio an attack should use.
type CombatDimension int

const (
	DimMelee CombatDimension = iota
	DimRanged
	DimMagic
)

// ResolvedStats groups the offensive stats from the attacker and defensive stats from the target
// for a single combat dimension.
type ResolvedStats struct {
	HitValue     int32 // attacker
	DmgMin       int32 // attacker
	DmgMax       int32 // attacker
	CritValue    int32 // attacker
	EvasionValue int32 // target
	DefenseValue int32 // target
}

// Default basic-attack range per dimension, used when a weapon has no explicit
// weapon_range (weapon_range == 0).
const (
	defaultMeleeRange  float32 = 2.0
	defaultRangedRange float32 = 15.0
	defaultMagicRange  float32 = 12.0
)

// DefaultRangeForDimension returns the fallback attack range for a dimension.
func DefaultRangeForDimension(dim CombatDimension) float32 {
	switch dim {
	case DimRanged:
		return defaultRangedRange
	case DimMagic:
		return defaultMagicRange
	default:
		return defaultMeleeRange
	}
}

// ResolveAttackRange picks the weapon's explicit range if set (>0), else the
// dimension default, then applies rangeBonusPct (DerivedStats.RangeBonusPct,
// PER-driven — see attributes.go) as a percentage increase on top: final =
// base * (1 + rangeBonusPct). Same convention already used by CooldownSpeedPct
// (attributes.go/combat_cooldown.go) for a *Pct derived stat modulating a base
// value, just increasing instead of decreasing.
func ResolveAttackRange(weaponRange float32, dim CombatDimension, rangeBonusPct float32) float32 {
	base := weaponRange
	if base <= 0 {
		base = DefaultRangeForDimension(dim)
	}
	if rangeBonusPct > 0 {
		base *= 1.0 + rangeBonusPct
	}
	return base
}

// resolveAbilityDimension returns the ability's combat dimension. If the
// ability has no explicit dimension (empty/unrecognized), it inherits the
// attacker's basic-attack dimension (the equipped weapon's dimension).
//
// Defined for C3b; not yet called from the combat runtime (C3a is data-only).
func resolveAbilityDimension(attacker *Actor, ability AbilityTemplate) CombatDimension {
	if dim, ok := parseCombatDimensionString(ability.Dimension); ok {
		return dim
	}
	// empty/unknown → inherit from attacker's weapon
	return attacker.BasicAttackDim
}

// parseCombatDimensionString parses the "melee"/"ranged"/"magic" string
// convention shared by AbilityTemplate.Dimension and
// AbilityTemplate.RequiredWeaponDimension. ok=false for empty/unrecognized
// input — callers treat that as "no explicit dimension", each with its own
// meaning (resolveAbilityDimension falls back to the attacker's weapon;
// canActorStartAbilityNow, cast_intent.go, treats it as "no restriction").
func parseCombatDimensionString(s string) (CombatDimension, bool) {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "melee":
		return DimMelee, true
	case "ranged":
		return DimRanged, true
	case "magic":
		return DimMagic, true
	default:
		return DimMelee, false
	}
}

// selectDimensionStats resolves the combat stats for a given dimension.
func selectDimensionStats(att, tgt DerivedStats, dim CombatDimension) ResolvedStats {
	switch dim {
	case DimRanged:
		return ResolvedStats{
			HitValue:     att.RangedHitValue,
			DmgMin:       att.RangedDmgMin,
			DmgMax:       att.RangedDmgMax,
			CritValue:    att.RangedCritValue,
			EvasionValue: tgt.RangedEvasionValue,
			DefenseValue: tgt.RangedDefenseValue,
		}
	case DimMagic:
		return ResolvedStats{
			HitValue:     att.MagicHitValue,
			DmgMin:       att.MagicDmgMin,
			DmgMax:       att.MagicDmgMax,
			CritValue:    att.MagicCritValue,
			EvasionValue: tgt.MagicEvasionValue,
			DefenseValue: tgt.MagicDefenseValue,
		}
	default:
		return ResolvedStats{
			HitValue:     att.MeleeHitValue,
			DmgMin:       att.MeleeDmgMin,
			DmgMax:       att.MeleeDmgMax,
			CritValue:    att.MeleeCritValue,
			EvasionValue: tgt.MeleeEvasionValue,
			DefenseValue: tgt.MeleeDefenseValue,
		}
	}
}

