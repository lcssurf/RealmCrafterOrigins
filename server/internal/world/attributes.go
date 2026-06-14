package world

// attributes.go is the central source of truth for character attributes:
// the canonical attribute registry (typing), the PrimaryStats/DerivedStats
// structs (typing), the balance coefficients, and the formulas that compute
// derived stats (HP, defense, crit, evasion, etc) from primary stats
// (STR, DEX, INT, WIS, PER) and level.
//
// To rebalance the game, edit the constants in this file. Combat-facing numbers
// should be defined here, not scattered across runtime code.
//
// Convention:
//   - Constants are grouped by stat category.
//   - Names follow the pattern: <stat>Per<Source>.
//   - Caps and minimums use the suffix Cap or Min.
//   - Percent-style fields use 0.0..1.0 internally (0.05 = 5%).

// =============================================================================
// ATTRIBUTE REGISTRY (typing)
// =============================================================================

// AttributeKind classifies how an item attribute should be interpreted.
type AttributeKind int

const (
	// AttributeKindPrimary affects PrimaryStats and should be folded into recomputation.
	AttributeKindPrimary AttributeKind = iota
	// AttributeKindDerived affects DerivedStats and may be applied as a post-compute overlay.
	AttributeKindDerived
)

// AttributeDef defines one canonical attribute key in the item bonus system.
type AttributeDef struct {
	Key         string
	DisplayName string
	Kind        AttributeKind
	IsFloat     bool
}

// AttributeRegistry is the canonical list of all attributes that can be granted by items.
var AttributeRegistry = []AttributeDef{
	// Primary stats
	{Key: "str", DisplayName: "Strength", Kind: AttributeKindPrimary, IsFloat: false},
	{Key: "dex", DisplayName: "Dexterity", Kind: AttributeKindPrimary, IsFloat: false},
	{Key: "int", DisplayName: "Intelligence", Kind: AttributeKindPrimary, IsFloat: false},
	{Key: "wis", DisplayName: "Wisdom", Kind: AttributeKindPrimary, IsFloat: false},
	{Key: "per", DisplayName: "Perception", Kind: AttributeKindPrimary, IsFloat: false},

	// Derived stats
	{Key: "health_max", DisplayName: "Health Max", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "health_regen", DisplayName: "Health Regen", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "energy_max", DisplayName: "Energy Max", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "energy_regen", DisplayName: "Energy Regen", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "melee_defense_value", DisplayName: "Melee Defense", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "ranged_defense_value", DisplayName: "Ranged Defense", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "magic_defense_value", DisplayName: "Magic Defense", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "melee_evasion_value", DisplayName: "Melee Evasion", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "ranged_evasion_value", DisplayName: "Ranged Evasion", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "magic_evasion_value", DisplayName: "Magic Evasion", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "melee_hit_value", DisplayName: "Melee Hit", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "ranged_hit_value", DisplayName: "Ranged Hit", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "magic_hit_value", DisplayName: "Magic Hit", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "melee_crit_value", DisplayName: "Melee Crit", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "ranged_crit_value", DisplayName: "Ranged Crit", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "magic_crit_value", DisplayName: "Magic Crit", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "melee_dmg_min", DisplayName: "Melee Dmg Min", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "melee_dmg_max", DisplayName: "Melee Dmg Max", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "ranged_dmg_min", DisplayName: "Ranged Dmg Min", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "ranged_dmg_max", DisplayName: "Ranged Dmg Max", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "magic_dmg_min", DisplayName: "Magic Dmg Min", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "magic_dmg_max", DisplayName: "Magic Dmg Max", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "crit_damage_mult", DisplayName: "Crit Damage Mult", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "attack_speed_mult", DisplayName: "Attack Speed Mult", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "movement_speed_mult", DisplayName: "Movement Speed Mult", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "cooldown_speed_pct", DisplayName: "Cooldown Speed %", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "skill_damage_boost_pct", DisplayName: "Skill Damage Boost %", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "buff_duration_pct", DisplayName: "Buff Duration %", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "debuff_duration_pct", DisplayName: "Debuff Duration %", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "range_bonus_pct", DisplayName: "Range Bonus %", Kind: AttributeKindDerived, IsFloat: true},
	{Key: "bonus_damage_flat", DisplayName: "Bonus Damage Flat", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "cc_chance_value", DisplayName: "CC Chance", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "cc_resistance_value", DisplayName: "CC Resistance", Kind: AttributeKindDerived, IsFloat: false},
	{Key: "damage_reduction_flat", DisplayName: "Damage Reduction Flat", Kind: AttributeKindDerived, IsFloat: false},
}

var attributeDefByKey map[string]AttributeDef

func init() {
	attributeDefByKey = make(map[string]AttributeDef, len(AttributeRegistry))
	for _, def := range AttributeRegistry {
		attributeDefByKey[def.Key] = def
	}
}

// AttributeByKey returns the attribute definition for a canonical key.
func AttributeByKey(key string) (AttributeDef, bool) {
	def, ok := attributeDefByKey[key]
	return def, ok
}

// =============================================================================
// STAT STRUCTS (typing)
// =============================================================================

// PrimaryStats represents the 5 base attributes of a character or NPC.
type PrimaryStats struct {
	STR int32 // Strength: HP, physical damage, physical defense
	DEX int32 // Dexterity: crit, evasion, attack speed
	INT int32 // Intelligence: mana, magic damage
	WIS int32 // Wisdom: cooldown reduction, healing, mana regen
	PER int32 // Perception: accuracy, CC chance, CC resistance
}

// DerivedStats holds all computed stats from primary + level + equipment.
//
// Conventions:
//   - "Value" suffix = integer accumulator (stacks with gear; converted to % via curve)
//   - "Pct" suffix = ratio 0.0..1.0 (already a percentage)
//   - "Mult" suffix = multiplier (1.0 = baseline)
//   - "Flat" suffix = flat additive value (raw integer/float)
type DerivedStats struct {
	// ===== Resources =====
	HealthMax   int32
	HealthRegen float32
	EnergyMax   int32
	EnergyRegen float32

	// ===== Defense (Value = integer accumulator, converted to % via curve) =====
	MeleeDefenseValue  int32
	RangedDefenseValue int32
	MagicDefenseValue  int32

	// ===== Evasion (Value) =====
	MeleeEvasionValue  int32
	RangedEvasionValue int32
	MagicEvasionValue  int32

	// ===== Hit Chance (Value, comparativo vs target evasion) =====
	MeleeHitValue  int32
	RangedHitValue int32
	MagicHitValue  int32

	// ===== Crit Chance (Value) =====
	MeleeCritValue  int32
	RangedCritValue int32
	MagicCritValue  int32

	// ===== Damage Range =====
	MeleeDmgMin  int32
	MeleeDmgMax  int32
	RangedDmgMin int32
	RangedDmgMax int32
	MagicDmgMin  int32
	MagicDmgMax  int32

	// ===== Crit Damage =====
	CritDamageMult float32

	// ===== Speed =====
	AttackSpeedMult   float32
	MovementSpeedMult float32
	CooldownSpeedPct  float32

	// ===== Modifiers =====
	SkillDamageBoostPct float32
	BuffDurationPct     float32
	DebuffDurationPct   float32

	// ===== Range & Damage Flat =====
	RangeBonusPct   float32
	BonusDamageFlat int32

	// ===== CC =====
	CCChanceValue     int32
	CCResistanceValue int32

	// ===== Damage Reduction =====
	DamageReductionFlat int32
}

// =============================================================================
// BALANCE CONSTANTS
// =============================================================================

// ----- Health -----
const (
	healthBase          int32   = 100
	healthPerSTR        int32   = 8
	healthPerLevel      int32   = 15
	healthRegenPerSTR   float32 = 0.3
	healthRegenPctOfMax float32 = 0.01 // 1%/s of HealthMax
)

// ----- Energy / Mana -----
const (
	energyBase          int32   = 50
	energyPerWIS        int32   = 4
	energyPerINT        int32   = 4
	energyPerLevel      int32   = 5
	energyRegenPerWIS   float32 = 0.2
	energyRegenPctOfMax float32 = 0.02 // 2%/s of EnergyMax
)

// ----- Defense (Value formulas) -----
const (
	meleeDefSTR  int32 = 5
	rangedDefSTR int32 = 2
	rangedDefDEX int32 = 2
	magicDefINT  int32 = 2
	magicDefWIS  int32 = 2
	defPerLevel  int32 = 2
)

// ----- Evasion (Value formulas) -----
const (
	meleeEvasionDEX  int32 = 4
	rangedEvasionDEX int32 = 5
	magicEvasionDEX  int32 = 2
	magicEvasionPER  int32 = 2
	evasionPerLevel  int32 = 1
)

// ----- Hit Chance (Value formulas) -----
const (
	hitPER       int32 = 10
	meleeHitSTR  int32 = 5
	rangedHitDEX int32 = 5
	magicHitINT  int32 = 5
	hitPerLevel  int32 = 2
)

// ----- Crit (Value formulas) -----
const (
	meleeCritDEX  int32 = 8
	rangedCritDEX int32 = 10
	magicCritDEX  int32 = 5
	magicCritINT  int32 = 5
	critPerLevel  int32 = 1
)

// ----- Melee Damage -----
const (
	meleeDmgMinFromSTR float32 = 1.5
	meleeDmgMaxFromSTR float32 = 2.0
	meleeDmgMaxFromDEX float32 = 0.5
)

// ----- Ranged Damage -----
const (
	rangedDmgMinFromDEX float32 = meleeDmgMinFromSTR
	rangedDmgMaxFromDEX float32 = meleeDmgMaxFromSTR
	rangedDmgMaxFromPER float32 = meleeDmgMaxFromDEX
)

// ----- Magic Damage -----
const (
	magicDmgMinFromINT float32 = 1.5
	magicDmgMaxFromINT float32 = 2.0
	magicDmgMaxFromPER float32 = 0.5
)

// ----- Crit Damage Multiplier -----
const (
	critDmgBase   float32 = 1.5
	critDmgPerDEX float32 = 0.01
	critDmgCap    float32 = 3.0
)

// ----- Speed -----
const (
	attackSpeedBase   float32 = 1.0
	attackSpeedPerDEX float32 = 0.005
	attackSpeedCap    float32 = 1.5

	moveSpeedBase   float32 = 1.0
	moveSpeedPerDEX float32 = 0.002
	moveSpeedCap    float32 = 1.3

	cooldownSpdPerWIS float32 = 0.002
	cooldownSpdCap    float32 = 0.30
)

// ----- Modifiers -----
const (
	skillDmgBoostPerINT float32 = 0.002
	skillDmgBoostPerWIS float32 = 0.001
	skillDmgBoostCap    float32 = 0.50

	buffDurationPerPER float32 = 0.003
	buffDurationCap    float32 = 0.50

	debuffDurationPerPER float32 = 0.003
	debuffDurationCap    float32 = 0.50
)

// ----- Range & Bonus Damage -----
const (
	rangeBonusPerPER float32 = 0.002
	rangeBonusCap    float32 = 0.30

	bonusDmgPerLevel int32 = 2
)

// ----- CC -----
const (
	ccChancePER      int32 = 5
	ccChancePerLevel int32 = 1

	ccResistancePER      int32 = 7
	ccResistancePerLevel int32 = 1
)

// -----------------------------------------------------------------------------
// Conversion curve constants
// -----------------------------------------------------------------------------
// These softcaps control how "Value" stats convert to percentages.
const (
	// Crit: 1500 -> 35% (cap 70%)
	critValueSoftcap int32   = 1500
	critValueCap     float32 = 0.70

	// Evasion: 2000 -> 30% (cap 60%)
	evasionSoftcap int32   = 2000
	evasionCap     float32 = 0.60

	// Defense: 3000 -> 40% (cap 80%)
	defenseSoftcap int32   = 3000
	defenseCap     float32 = 0.80

	// CC: 1500 -> 30% (cap 60%)
	ccValueSoftcap int32   = 1500
	ccValueCap     float32 = 0.60
)

// =============================================================================
// STAT COMPUTATION
// =============================================================================

// ComputeDerivedStats applies all derived stat formulas in a single pass.
// Pure function with no side effects.
func ComputeDerivedStats(primary PrimaryStats, level int32, weaponDmg int32, armor int32) DerivedStats {
	var d DerivedStats

	// Resources
	d.HealthMax = healthBase + primary.STR*healthPerSTR + level*healthPerLevel
	// Mixed regen: proportional to max HP (fair for tanks) + flat bonus from STR.
	d.HealthRegen = float32(d.HealthMax)*healthRegenPctOfMax + float32(primary.STR)*healthRegenPerSTR
	d.EnergyMax = energyBase + primary.WIS*energyPerWIS + primary.INT*energyPerINT + level*energyPerLevel
	// Mixed regen: proportional to max energy + flat bonus from WIS.
	d.EnergyRegen = float32(d.EnergyMax)*energyRegenPctOfMax + float32(primary.WIS)*energyRegenPerWIS

	// Defense values
	d.MeleeDefenseValue = primary.STR*meleeDefSTR + level*defPerLevel + armor
	d.RangedDefenseValue = primary.STR*rangedDefSTR + primary.DEX*rangedDefDEX + level*defPerLevel + armor
	d.MagicDefenseValue = primary.INT*magicDefINT + primary.WIS*magicDefWIS + level*defPerLevel + armor

	// Evasion values
	d.MeleeEvasionValue = primary.DEX*meleeEvasionDEX + level*evasionPerLevel
	d.RangedEvasionValue = primary.DEX*rangedEvasionDEX + level*evasionPerLevel
	d.MagicEvasionValue = primary.DEX*magicEvasionDEX + primary.PER*magicEvasionPER + level*evasionPerLevel

	// Hit values
	d.MeleeHitValue = primary.PER*hitPER + primary.STR*meleeHitSTR + level*hitPerLevel
	d.RangedHitValue = primary.PER*hitPER + primary.DEX*rangedHitDEX + level*hitPerLevel
	d.MagicHitValue = primary.PER*hitPER + primary.INT*magicHitINT + level*hitPerLevel

	// Crit values
	d.MeleeCritValue = primary.DEX*meleeCritDEX + level*critPerLevel
	d.RangedCritValue = primary.DEX*rangedCritDEX + level*critPerLevel
	d.MagicCritValue = primary.DEX*magicCritDEX + primary.INT*magicCritINT + level*critPerLevel

	// Damage ranges
	d.MeleeDmgMin, d.MeleeDmgMax = MeleeDamageRange(primary, weaponDmg)
	d.RangedDmgMin, d.RangedDmgMax = RangedDamageRange(primary, weaponDmg)
	d.MagicDmgMin, d.MagicDmgMax = MagicDamageRange(primary, weaponDmg)

	// Crit damage multiplier
	d.CritDamageMult = clampFloat(critDmgBase+float32(primary.DEX)*critDmgPerDEX, critDmgBase, critDmgCap)

	// Speed
	d.AttackSpeedMult = clampFloat(attackSpeedBase+float32(primary.DEX)*attackSpeedPerDEX, attackSpeedBase, attackSpeedCap)
	d.MovementSpeedMult = clampFloat(moveSpeedBase+float32(primary.DEX)*moveSpeedPerDEX, moveSpeedBase, moveSpeedCap)
	d.CooldownSpeedPct = clampFloat(float32(primary.WIS)*cooldownSpdPerWIS, 0, cooldownSpdCap)

	// Modifiers
	d.SkillDamageBoostPct = clampFloat(float32(primary.INT)*skillDmgBoostPerINT+float32(primary.WIS)*skillDmgBoostPerWIS, 0, skillDmgBoostCap)
	d.BuffDurationPct = clampFloat(float32(primary.PER)*buffDurationPerPER, 0, buffDurationCap)
	d.DebuffDurationPct = clampFloat(float32(primary.PER)*debuffDurationPerPER, 0, debuffDurationCap)

	// Range and flat damage
	d.RangeBonusPct = clampFloat(float32(primary.PER)*rangeBonusPerPER, 0, rangeBonusCap)
	d.BonusDamageFlat = level * bonusDmgPerLevel

	// CC values
	d.CCChanceValue = primary.PER*ccChancePER + level*ccChancePerLevel
	d.CCResistanceValue = primary.PER*ccResistancePER + level*ccResistancePerLevel

	// Gear/buffs fill this later.
	d.DamageReductionFlat = 0

	return d
}

// MeleeDamageRange returns min/max melee damage from primary stats + weapon.
func MeleeDamageRange(primary PrimaryStats, weaponDmg int32) (min int32, max int32) {
	min = weaponDmg + int32(float32(primary.STR)*meleeDmgMinFromSTR)
	max = weaponDmg + int32(float32(primary.STR)*meleeDmgMaxFromSTR+float32(primary.DEX)*meleeDmgMaxFromDEX)
	if min > max {
		max = min
	}
	return
}

// RangedDamageRange returns min/max ranged damage from primary stats + weapon.
func RangedDamageRange(primary PrimaryStats, weaponDmg int32) (min int32, max int32) {
	min = weaponDmg + int32(float32(primary.DEX)*rangedDmgMinFromDEX)
	max = weaponDmg + int32(float32(primary.DEX)*rangedDmgMaxFromDEX+float32(primary.PER)*rangedDmgMaxFromPER)
	if min > max {
		max = min
	}
	return
}

// MagicDamageRange returns min/max magic damage from primary stats + weapon.
func MagicDamageRange(primary PrimaryStats, weaponDmg int32) (min int32, max int32) {
	min = weaponDmg + int32(float32(primary.INT)*magicDmgMinFromINT)
	max = weaponDmg + int32(float32(primary.INT)*magicDmgMaxFromINT+float32(primary.PER)*magicDmgMaxFromPER)
	if min > max {
		max = min
	}
	return
}

// ValueToPercent converts an accumulated integer stat value to a percentage
// using a softcap curve:
//
//	pct = cap * value / (value + softcap)
//
// Returns 0 if value <= 0 and never exceeds cap.
func ValueToPercent(value int32, cap float32, softcap int32) float32 {
	if value <= 0 {
		return 0
	}
	pct := cap * float32(value) / float32(value+softcap)
	if pct > cap {
		return cap
	}
	return pct
}

// clampFloat clamps a float32 value to [min, max].
func clampFloat(v, min, max float32) float32 {
	if v < min {
		return min
	}
	if v > max {
		return max
	}
	return v
}

// =============================================================================
// ITEM BONUS
// =============================================================================

// applyDerivedBonus adds an item bonus to the matching DerivedStats field.
// Unknown keys are ignored. int32 fields truncate the float value.
func applyDerivedBonus(d *DerivedStats, key string, value float64) {
	switch key {
	case "health_max":
		d.HealthMax += int32(value)
	case "health_regen":
		d.HealthRegen += float32(value)
	case "energy_max":
		d.EnergyMax += int32(value)
	case "energy_regen":
		d.EnergyRegen += float32(value)
	case "melee_defense_value":
		d.MeleeDefenseValue += int32(value)
	case "ranged_defense_value":
		d.RangedDefenseValue += int32(value)
	case "magic_defense_value":
		d.MagicDefenseValue += int32(value)
	case "melee_evasion_value":
		d.MeleeEvasionValue += int32(value)
	case "ranged_evasion_value":
		d.RangedEvasionValue += int32(value)
	case "magic_evasion_value":
		d.MagicEvasionValue += int32(value)
	case "melee_hit_value":
		d.MeleeHitValue += int32(value)
	case "ranged_hit_value":
		d.RangedHitValue += int32(value)
	case "magic_hit_value":
		d.MagicHitValue += int32(value)
	case "melee_crit_value":
		d.MeleeCritValue += int32(value)
	case "ranged_crit_value":
		d.RangedCritValue += int32(value)
	case "magic_crit_value":
		d.MagicCritValue += int32(value)
	case "melee_dmg_min":
		d.MeleeDmgMin += int32(value)
	case "melee_dmg_max":
		d.MeleeDmgMax += int32(value)
	case "ranged_dmg_min":
		d.RangedDmgMin += int32(value)
	case "ranged_dmg_max":
		d.RangedDmgMax += int32(value)
	case "magic_dmg_min":
		d.MagicDmgMin += int32(value)
	case "magic_dmg_max":
		d.MagicDmgMax += int32(value)
	case "crit_damage_mult":
		d.CritDamageMult += float32(value)
	case "attack_speed_mult":
		d.AttackSpeedMult += float32(value)
	case "movement_speed_mult":
		d.MovementSpeedMult += float32(value)
	case "cooldown_speed_pct":
		d.CooldownSpeedPct += float32(value)
	case "skill_damage_boost_pct":
		d.SkillDamageBoostPct += float32(value)
	case "buff_duration_pct":
		d.BuffDurationPct += float32(value)
	case "debuff_duration_pct":
		d.DebuffDurationPct += float32(value)
	case "range_bonus_pct":
		d.RangeBonusPct += float32(value)
	case "bonus_damage_flat":
		d.BonusDamageFlat += int32(value)
	case "cc_chance_value":
		d.CCChanceValue += int32(value)
	case "cc_resistance_value":
		d.CCResistanceValue += int32(value)
	case "damage_reduction_flat":
		d.DamageReductionFlat += int32(value)
	}
}

// =============================================================================
// RECOMPUTE
// =============================================================================

// RecomputeDerivedStats rebuilds actor.Derived from current primary stats,
// level and equipped combat stats, then syncs legacy max resource fields.
//
// itemBonuses is the aggregated map of item_attributes from currently
// equipped items (see db.GetEquippedAttributes), or nil if none apply.
// Primary-kind bonuses (str/dex/int/wis/per) are folded into a local
// effectivePrimary used only for this computation — actor.Primary (the
// persisted base) is never modified. Derived-kind bonuses are applied as an
// overlay on top of the computed DerivedStats.
//
// This function updates only HealthMax/EnergyMax. It intentionally does not
// modify current Health/Energy values.
func RecomputeDerivedStats(actor *Actor, itemBonuses map[string]float64) {
	if actor == nil {
		return
	}

	actor.Mu.Lock()
	primary := actor.Primary
	level := int32(actor.Level)
	weaponDmg := actor.WeaponDamage
	armor := actor.CachedArmor
	actor.Mu.Unlock()

	// Primary-kind item bonuses: folded into a local copy, base untouched.
	effectivePrimary := primary
	if itemBonuses != nil {
		effectivePrimary.STR += int32(itemBonuses["str"])
		effectivePrimary.DEX += int32(itemBonuses["dex"])
		effectivePrimary.INT += int32(itemBonuses["int"])
		effectivePrimary.WIS += int32(itemBonuses["wis"])
		effectivePrimary.PER += int32(itemBonuses["per"])
	}

	derived := ComputeDerivedStats(effectivePrimary, level, weaponDmg, armor)

	// Derived-kind item bonuses: overlay applied after compute.
	if itemBonuses != nil {
		for key, val := range itemBonuses {
			if def, ok := AttributeByKey(key); ok && def.Kind == AttributeKindDerived {
				applyDerivedBonus(&derived, key, val)
			}
		}
	}

	actor.Mu.Lock()
	actor.Derived = derived
	actor.EffectivePrimary = effectivePrimary
	actor.HealthMax = derived.HealthMax
	actor.EnergyMax = derived.EnergyMax
	actor.Mu.Unlock()
}
