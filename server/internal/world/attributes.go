package world

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
