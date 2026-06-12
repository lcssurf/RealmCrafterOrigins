// Package world - combat_special.go
//
// Special ability windup, resolution, and damage helpers.
package world

import (
	"fmt"
	"log"
	"math"
	"math/rand"
	"strings"
)

// resolveActorWindup resolves a previously armed special windup for any actor.
//
// Returns handled=true when an active windup was present (including while still
// charging). killedTarget is true when the impact kills the current target.
func resolveActorWindup(area *Area, actor, target *Actor, now int64) (handled bool, killedTarget bool) {
	if area == nil || actor == nil || target == nil {
		return false, false
	}

	actor.Mu.Lock()
	windupUntil := actor.SpecialWindupUntil
	windupTarget := actor.SpecialTargetRID
	windupAbilityID := actor.SpecialAbilityID
	windupActionOverride := actor.SpecialActionOverride
	actor.Mu.Unlock()

	if windupUntil <= 0 {
		return false, false
	}
	if now < windupUntil {
		return true, false
	}
	ability := resolveSpecialAbilityTemplate(windupAbilityID)

	// Clear windup first so impact resolves exactly once.
	actor.Mu.Lock()
	actor.SpecialWindupUntil = 0
	actor.SpecialTargetRID = 0
	actor.SpecialAbilityID = 0
	actor.SpecialActionOverride = ""
	actor.SpecialReasonTag = ""
	actor.SpecialClientTraceID = ""
	actor.Mu.Unlock()

	// If target changed mid-windup, resolve against the originally telegraphed target.
	if windupTarget != 0 {
		if target == nil || target.RuntimeID != windupTarget {
			forced, ok := area.GetActor(windupTarget)
			if !ok || forced == nil {
				if recover := resolveStageAction(windupActionOverride, ability.ActionRecover, "Idle"); recover != "" {
					BroadcastAnimate(area, actor, recover)
				}
				log.Printf("special: cancelled cast actor=%d ability=%d reason=target_missing",
					actor.RuntimeID, windupAbilityID)
				return true, false
			}
			target = forced
		}
	}
	if target == nil || target.IsDead() {
		if recover := resolveStageAction(windupActionOverride, ability.ActionRecover, "Idle"); recover != "" {
			BroadcastAnimate(area, actor, recover)
		}
		log.Printf("special: cancelled cast actor=%d ability=%d reason=target_dead",
			actor.RuntimeID, windupAbilityID)
		return true, false
	}
	if !inSpecialRange(actor, target, ability.RangeMin, ability.RangeMax) {
		// Target escaped the special impact radius.
		if recover := resolveStageAction(windupActionOverride, ability.ActionRecover, "Idle"); recover != "" {
			BroadcastAnimate(area, actor, recover)
		}
		log.Printf("special: cancelled cast actor=%d ability=%d target=%d reason=out_of_range",
			actor.RuntimeID, windupAbilityID, target.RuntimeID)
		return true, false
	}

	target.Mu.Lock()
	parryActive := target.ParryUntil > now
	parryAge := now - target.LastParryAt
	parryWindow := ability.ParryWindowMs
	if parryWindow <= 0 {
		parryWindow = npcSpecialParryExactMs
	}
	if parryActive && parryAge >= 0 && parryAge <= parryWindow {
		// Consume the parry so the same window doesn't double-count.
		target.ParryUntil = 0
		target.LastCombatAt = now
		target.Mu.Unlock()

		BroadcastCombatEvent(area, combatEventSpecialParry, actor.RuntimeID, target.RuntimeID, int16(parryAge), "")
		if recover := resolveStageAction(windupActionOverride, ability.ActionRecover, "Idle"); recover != "" {
			BroadcastAnimate(area, actor, recover)
		}
		return true, false
	}
	target.Mu.Unlock()

	if impact := resolveStageAction(windupActionOverride, ability.ActionImpact, "Attack"); impact != "" {
		BroadcastAnimate(area, actor, impact)
	}
	damage, isCrit := specialAttackDamage(actor, target, ability)
	hp, justDied := ApplyDamage(target, damage, actor.RuntimeID)
	if isCrit {
		BroadcastFloatingNumber(area, target, int16(damage), 1)
		BroadcastCombatEvent(area, combatEventSpecialCritHit, actor.RuntimeID, target.RuntimeID, int16(damage), "")
	} else {
		BroadcastFloatingNumber(area, target, int16(damage), 0)
		BroadcastCombatEvent(area, combatEventSpecialHit, actor.RuntimeID, target.RuntimeID, int16(damage), "")
	}
	if damage > 0 && target.IsNPC && GetBloodMode() == "all" {
		if bloodFX := GetBloodFX(); bloodFX != "" {
			BroadcastBloodFX(area, actor, target, bloodFX)
		}
	}
	BroadcastHPUpdate(area, target, hp)
	BroadcastAbilityFX(area, actor, target, ability, FXPhaseImpact)
	if !actor.IsNPC && actor.CharacterID != "" && target.IsNPC {
		GetCombatWindowManager().TrackSkill(
			actor.RuntimeID,
			target.RuntimeID,
			uint32(windupAbilityID),
			int32(target.Level),
			actor.CharacterID,
		)
	}
	if justDied {
		BroadcastAnimate(area, target, "Death")
		BroadcastActorDead(area, target.RuntimeID, actor.RuntimeID)
		OnNPCKilled(area, target, actor.RuntimeID)
		runSpecialKillHook(area, actor, target)
	}
	return true, justDied
}

// ProcessNPCSpecialAttack runs the NPC "special/parry-check" flow.
//
// Returns handled=true when a special is active/started/resolved and normal melee
// should be skipped this tick. killedTarget is true when the special impact kills
// the current target player.
func ProcessNPCSpecialAttack(area *Area, npc, target *Actor, now int64) (handled bool, killedTarget bool) {
	if area == nil || npc == nil || target == nil {
		return false, false
	}

	npc.Mu.Lock()
	lastSpecialAt := npc.LastSpecialAt
	if npc.AbilityCooldowns == nil {
		npc.AbilityCooldowns = make(map[int]int64)
	}
	npc.Mu.Unlock()

	// 1) Resolve active windup (shared flow for any actor).
	if handled, killed := resolveActorWindup(area, npc, target, now); handled {
		return true, killed
	}

	// 2) No active windup: NPC-only decision and cast start.
	intent, ok := selectNPCSpecialIntent(npc, target, now, lastSpecialAt)
	if !ok {
		return false, false
	}
	if !startNPCSpecialCast(area, npc, target, intent.Ability, "", "npc_ai", "", now) {
		return false, false
	}
	return true, false
}

func resolveSpecialAbilityTemplate(abilityID int) AbilityTemplate {
	if abilityID > 0 {
		if tmpl, ok := resolveAbilityTemplate(abilityID); ok && tmpl.Enabled {
			return tmpl
		}
	}
	return legacySpecialAbilityTemplate()
}

func legacySpecialAbilityTemplate() AbilityTemplate {
	return AbilityTemplate{
		ID:            0,
		Name:          "Legacy Special",
		CooldownMs:    npcSpecialCooldownMsLegacy,
		WindupMs:      npcSpecialWindupMsLegacy,
		ParryWindowMs: npcSpecialParryExactMs,
		RangeMin:      0,
		RangeMax:      0,
		ActionWindup:  "Attack",
		ActionImpact:  "Attack",
		ActionRecover: "Idle",
		Enabled:       true,
	}
}

func resolveStageAction(actionOverride, preferred, fallback string) string {
	if actionOverride != "" {
		return actionOverride
	}
	if preferred != "" {
		return preferred
	}
	if fallback != "" {
		return fallback
	}
	return "Attack"
}

func resolveSpecialMinRange(slot NPCAbilityLoadoutEntry, ability AbilityTemplate) float32 {
	if slot.MinDistance > 0 {
		return slot.MinDistance
	}
	if ability.RangeMin > 0 {
		return ability.RangeMin
	}
	return 0
}

func resolveSpecialMaxRange(npc *Actor, slot NPCAbilityLoadoutEntry, ability AbilityTemplate) float32 {
	if slot.MaxDistance > 0 {
		return slot.MaxDistance
	}
	if ability.RangeMax > 0 {
		return ability.RangeMax
	}
	if npc.AttackRange > 0 {
		return npc.AttackRange
	}
	return MeleeRange
}

func inSpecialRange(npc, target *Actor, minDistance, maxDistance float32) bool {
	dx := float64(npc.X - target.X)
	dz := float64(npc.Z - target.Z)
	dy := float64(npc.Y-target.Y) / 5.0
	dist := float32(math.Sqrt(dx*dx + dz*dz + dy*dy))
	maxRange := maxDistance
	if maxRange <= 0 {
		if npc.AttackRange > 0 {
			maxRange = npc.AttackRange
		} else {
			maxRange = MeleeRange
		}
	}
	minRange := minDistance
	if minRange < 0 {
		minRange = 0
	}
	// Radii are part of combat reach; subtract them from minimum and add to maximum.
	reach := npc.Radius + target.Radius
	if minRange > reach {
		minRange -= reach
	} else {
		minRange = 0
	}
	maxRange += reach
	return dist >= minRange && dist <= maxRange
}

func isTargetHPAllowed(target *Actor, minPct, maxPct float32) bool {
	target.Mu.Lock()
	hp := target.Health
	hpMax := target.HealthMax
	target.Mu.Unlock()
	if hpMax <= 0 {
		return false
	}
	pct := (float32(hp) / float32(hpMax)) * 100.0
	lo := minPct
	hi := maxPct
	if lo < 0 {
		lo = 0
	}
	if hi <= 0 || hi > 100 {
		hi = 100
	}
	if hi < lo {
		hi = lo
	}
	return pct >= lo && pct <= hi
}

func startNPCSpecialCast(
	area *Area,
	npc *Actor,
	target *Actor,
	ability AbilityTemplate,
	actionOverride string,
	reasonTag string,
	clientTraceID string,
	now int64,
) bool {
	if area == nil || npc == nil || target == nil {
		return false
	}
	windupMs := ability.WindupMs
	if windupMs <= 0 {
		windupMs = npcSpecialWindupMsLegacy
	}

	npc.Mu.Lock()
	previousLastSpecialAt := npc.LastSpecialAt
	if npc.AbilityCooldowns == nil {
		npc.AbilityCooldowns = make(map[int]int64)
	}
	markNPCSpecialChainCastStarted(npc, now, previousLastSpecialAt)
	npc.SpecialWindupUntil = now + windupMs
	npc.SpecialTargetRID = target.RuntimeID
	npc.SpecialAbilityID = ability.ID
	npc.SpecialActionOverride = actionOverride
	npc.SpecialReasonTag = reasonTag
	npc.SpecialClientTraceID = clientTraceID
	npc.LastSpecialAt = now
	npc.LastCombatAt = now
	if ability.ID > 0 {
		npc.AbilityCooldowns[ability.ID] = now
	}
	npc.Mu.Unlock()

	BroadcastCombatEvent(
		area,
		combatEventSpecialWindup,
		npc.RuntimeID,
		target.RuntimeID,
		int16(windupMs),
		buildSpecialWindupMetaText(ability, reasonTag, clientTraceID),
	)
	if windupAction := resolveStageAction(actionOverride, ability.ActionWindup, "Attack"); windupAction != "" {
		BroadcastAnimate(area, npc, windupAction)
	}
	BroadcastAbilityFX(area, npc, target, ability, FXPhaseWindup)
	return true
}

func specialAttackDamage(npc, target *Actor, ability AbilityTemplate) (int32, bool) {
	npc.Mu.Lock()
	fallbackBase := npc.WeaponDamage*2 + npc.Strength/2 + int32(npc.Level)*2
	npcDerived := npc.Derived
	npc.Mu.Unlock()

	target.Mu.Lock()
	armor := target.CachedArmor
	hpMax := target.HealthMax
	targetDerived := target.Derived
	target.Mu.Unlock()

	baseMin := ability.BaseDamageMin
	baseMax := ability.BaseDamageMax
	if baseMin <= 0 && baseMax <= 0 {
		baseMin = npcDerived.MeleeDmgMin
		baseMax = npcDerived.MeleeDmgMax
		if baseMin <= 0 && baseMax <= 0 {
			baseMin = fallbackBase
			baseMax = fallbackBase
		}
	}
	if baseMin > baseMax {
		baseMin, baseMax = baseMax, baseMin
	}
	var base int32
	base = baseMin
	if baseMax > baseMin {
		base += rand.Int31n(baseMax - baseMin + 1)
	}
	// Apply optional per-skill additive scaling configured via JSON.
	if ability.DamageStatScale != nil {
		for _, entry := range ability.DamageStatScale.Scaling {
			statValue := getStatValueForScaling(npc, entry.Stat)
			base += int32(float32(statValue) * entry.Coef)
		}
	}

	pierce := ability.ArmorPiercePct
	if pierce < 0 {
		pierce = 0
	}
	if pierce > 100 {
		pierce = 100
	}
	effectiveArmor := int32(float32(armor) * (1.0 - pierce/100.0))
	if effectiveArmor < 0 {
		effectiveArmor = 0
	}
	dmg := base - effectiveArmor/2
	if dmg < 1 {
		dmg = 1
	}
	defPct := ValueToPercent(targetDerived.MeleeDefenseValue, defenseCap, defenseSoftcap)
	dmg = int32(float32(dmg) * (1.0 - defPct))
	if dmg < 1 {
		dmg = 1
	}

	var critPct float32
	var critMult float32
	if ability.CritPolicy != nil {
		cp := ability.CritPolicy
		statValue := getStatValueForScaling(npc, cp.ScalingStat)
		scaledPct := ValueToPercent(statValue, cp.ScalingSoftcapPct, cp.ScalingSoftcapValue)
		critPct = (cp.BaseChancePct / 100.0) + scaledPct
		critMult = cp.DamageMultiplier
		if critMult < 1 {
			critMult = 1
		}
	} else {
		critPct = ValueToPercent(npcDerived.MeleeCritValue, critValueCap, critValueSoftcap)
		critMult = npcDerived.CritDamageMult
		if critMult < 1 {
			critMult = 1
		}
	}
	isCrit := rand.Float32() < critPct
	if isCrit {
		dmg = int32(float32(dmg) * critMult)
		if dmg < 1 {
			dmg = 1
		}
	}

	// Legacy fallback keeps special attacks threatening while content migrates.
	if ability.ID == 0 {
		if dmg < npcSpecialMinDamage {
			dmg = npcSpecialMinDamage
		}
		hpFloor := hpMax / 3
		if hpFloor < npcSpecialMinDamage {
			hpFloor = npcSpecialMinDamage
		}
		if dmg < hpFloor {
			dmg = hpFloor
		}
		return dmg, isCrit
	}

	if ability.BaseDamageMin > 0 && dmg < ability.BaseDamageMin {
		dmg = ability.BaseDamageMin
	}

	// Player mastery runtime currently applies only to damage-category skills.
	if !npc.IsNPC && npc.CharacterID != "" && strings.EqualFold(strings.TrimSpace(ability.Category), "damage") {
		level := getPlayerSkillLevel(npc, ability.ID)
		if level > 1 {
			levelBonus := float64(level - 1)
			dmgMul := 1.0 + ability.MasteryPrimaryBonusPerLvl*levelBonus
			dmg = int32(float64(dmg) * dmgMul)
			if dmg < 1 {
				dmg = 1
			}
		}
	}
	return dmg, isCrit
}

func getStatValueForScaling(actor *Actor, statName string) int32 {
	if actor == nil {
		return 0
	}
	actor.Mu.Lock()
	defer actor.Mu.Unlock()
	switch strings.ToUpper(strings.TrimSpace(statName)) {
	case "STR":
		return actor.Primary.STR
	case "DEX":
		return actor.Primary.DEX
	case "INT":
		return actor.Primary.INT
	case "WIS":
		return actor.Primary.WIS
	case "PER":
		return actor.Primary.PER
	case "LEVEL":
		return int32(actor.Level)
	}
	return 0
}

func buildSpecialWindupMetaText(ability AbilityTemplate, reasonTag, clientTraceID string) string {
	reason := sanitizeCombatMetaValue(reasonTag)
	if reason == "" {
		reason = "npc_ai"
	}
	style := sanitizeCombatMetaValue(ability.TelegraphType)
	if style == "" {
		style = "ring_close"
	}
	color := sanitizeCombatMetaValue(ability.TelegraphColorRGBA)
	if color == "" {
		color = "1,0.2,0.2,0.75"
	}
	radius := ability.TelegraphRadius
	if radius <= 0 {
		radius = 1.45
	}
	parryWindowMs := ability.ParryWindowMs
	if parryWindowMs <= 0 {
		parryWindowMs = npcSpecialParryExactMs
	}
	trace := sanitizeCombatMetaValue(clientTraceID)
	if trace == "" {
		return fmt.Sprintf(
			"meta:telegraph=parry;ability=%d;reason=%s;radius=%.2f;color=%s;style=%s;window_ms=%d",
			ability.ID, reason, radius, color, style, parryWindowMs,
		)
	}
	return fmt.Sprintf(
		"meta:telegraph=parry;ability=%d;reason=%s;radius=%.2f;color=%s;style=%s;window_ms=%d;trace=%s",
		ability.ID, reason, radius, color, style, parryWindowMs, trace,
	)
}
