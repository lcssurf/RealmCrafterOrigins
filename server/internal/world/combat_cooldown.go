// Package world - combat_cooldown.go
//
// Cooldown and mastery-driven cooldown adjustment helpers.
package world

import (
	"strings"
)

func getPlayerSkillLevel(actor *Actor, abilityID int) int {
	if actor == nil || abilityID <= 0 {
		return 1
	}
	actor.Mu.Lock()
	defer actor.Mu.Unlock()
	if actor.SkillLevels == nil {
		return 1
	}
	level, ok := actor.SkillLevels[abilityID]
	if !ok || level < 1 {
		return 1
	}
	return level
}

func effectiveCooldownMs(actor *Actor, ability AbilityTemplate) int64 {
	base := ability.CooldownMs
	if base <= 0 {
		return base
	}
	if actor == nil || actor.IsNPC || actor.CharacterID == "" {
		return base
	}
	if !strings.EqualFold(strings.TrimSpace(ability.Category), "damage") {
		return base
	}

	level := getPlayerSkillLevel(actor, ability.ID)
	if level <= 1 {
		return base
	}

	levelBonus := float64(level - 1)
	cdMul := 1.0 - ability.MasteryCooldownReduxPerLvl*levelBonus
	if cdMul < 0.1 {
		cdMul = 0.1
	}
	return int64(float64(base) * cdMul)
}

// EffectiveCooldownMs returns the runtime cooldown for the actor+ability pair,
// including mastery cooldown reduction rules for player damage abilities.
func EffectiveCooldownMs(actor *Actor, ability AbilityTemplate) int64 {
	return effectiveCooldownMs(actor, ability)
}

func abilityOnCooldown(npc *Actor, ability AbilityTemplate, now int64) bool {
	if ability.ID <= 0 {
		return false
	}
	cooldownMs := effectiveCooldownMs(npc, ability)
	if cooldownMs <= 0 {
		return false
	}
	npc.Mu.Lock()
	last := npc.AbilityCooldowns[ability.ID]
	npc.Mu.Unlock()
	return now-last < cooldownMs
}

