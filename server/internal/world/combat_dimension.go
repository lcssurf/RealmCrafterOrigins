package world

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
// dimension default.
func ResolveAttackRange(weaponRange float32, dim CombatDimension) float32 {
	if weaponRange > 0 {
		return weaponRange
	}
	return DefaultRangeForDimension(dim)
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

