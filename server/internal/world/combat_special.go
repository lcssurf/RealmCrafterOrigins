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

// ResolveActorPendingImpacts resolves every one of actor.PendingImpacts whose
// ResolveAt has passed — each entry independently, using ITS OWN TargetRID
// (0 = no personal target, e.g. NPCCombat.spawn_impact) rather than a single
// caller-supplied target. Entries not yet due stay in the list untouched.
// Replaces the old resolveActorWindup, which only ever handled ONE scalar
// windup per actor — see Actor.PendingImpacts doc (actor.go) and
// docs/TECH_DEBT.md, mob AoE + multi-impact investigation.
//
// Returns handled=true if at least one GatesAbilityCasts=true entry was
// either still charging or resolved THIS call — mirrors the old
// windupUntil>0 semantics for "skip normal melee this tick" (ProcessNPCSpecialAttack).
// Raw spawn_impact entries (GatesAbilityCasts=false) never contribute —
// meteors falling in the background must not stall the boss's normal attack
// pattern. killedAny is true if any impact resolved this call killed its
// primary target (0 for target-less entries never counts here).
func ResolveActorPendingImpacts(area *Area, actor *Actor, now int64) (handled bool, killedAny bool) {
	if area == nil || actor == nil {
		return false, false
	}

	actor.Mu.Lock()
	pending := actor.PendingImpacts
	actor.Mu.Unlock()
	if len(pending) == 0 {
		return false, false
	}

	var stillCharging []PendingImpact
	for _, entry := range pending {
		if entry.GatesAbilityCasts {
			handled = true // charging or about to resolve this call — either way, gates melee this tick
		}
		if now < entry.ResolveAt {
			stillCharging = append(stillCharging, entry)
			continue
		}
		if resolvePendingImpactEntry(area, actor, entry, now) {
			killedAny = true
		}
	}

	actor.Mu.Lock()
	actor.PendingImpacts = stillCharging
	actor.Mu.Unlock()

	return handled, killedAny
}

// resolvePendingImpactEntry resolves ONE due PendingImpact. Entries with a
// TargetRID run the full personal-target flow (missing/dead/out-of-range
// cancel, dodge, parry — unchanged from the old single-windup path).
// Entries with TargetRID==0 (raw spawn_impact) skip all of that — there is
// no "the target" to escape or react to a splash — and go straight to the
// AoE resolution, where every hostile actor in radius (including what would
// otherwise be a "primary" target) is treated uniformly.
func resolvePendingImpactEntry(area *Area, actor *Actor, entry PendingImpact, now int64) (killedTarget bool) {
	ability := entry.Ability

	var primary *Actor
	if entry.TargetRID != 0 {
		forced, ok := area.GetActor(entry.TargetRID)
		if !ok || forced == nil {
			if recover := resolveStageAction(entry.ActionOverride, ability.ActionRecover, "Idle"); recover != "" {
				BroadcastAnimate(area, actor, recover)
			}
			log.Printf("special: cancelled impact=%d actor=%d ability=%d reason=target_missing",
				entry.ID, actor.RuntimeID, ability.ID)
			return false
		}
		primary = forced
		if primary.IsDead() {
			if recover := resolveStageAction(entry.ActionOverride, ability.ActionRecover, "Idle"); recover != "" {
				BroadcastAnimate(area, actor, recover)
			}
			log.Printf("special: cancelled impact=%d actor=%d ability=%d reason=target_dead",
				entry.ID, actor.RuntimeID, ability.ID)
			return false
		}
		if !inSpecialRange(actor, primary, ability.RangeMin, ability.RangeMax) {
			if recover := resolveStageAction(entry.ActionOverride, ability.ActionRecover, "Idle"); recover != "" {
				BroadcastAnimate(area, actor, recover)
			}
			log.Printf("special: cancelled impact=%d actor=%d ability=%d target=%d reason=out_of_range",
				entry.ID, actor.RuntimeID, ability.ID, primary.RuntimeID)
			return false
		}

		primary.Mu.Lock()
		if primary.DodgeUntil > now {
			primary.LastCombatAt = now
			primary.Mu.Unlock()

			BroadcastCombatEvent(area, combatEventHitDodged, actor.RuntimeID, primary.RuntimeID, 0,
				buildImpactResolutionMetaText(entry.ID))
			if recover := resolveStageAction(entry.ActionOverride, ability.ActionRecover, "Idle"); recover != "" {
				BroadcastAnimate(area, actor, recover)
			}
			return false
		}
		primary.Mu.Unlock()

		primary.Mu.Lock()
		parryActive := primary.ParryUntil > now
		parryAge := now - primary.LastParryAt
		parryWindow := ability.ParryWindowMs
		if parryWindow <= 0 {
			parryWindow = npcSpecialParryExactMs
		}
		if parryActive && parryAge >= 0 && parryAge <= parryWindow {
			// Consume the parry so the same window doesn't double-count.
			primary.ParryUntil = 0
			primary.LastCombatAt = now
			primary.Mu.Unlock()

			BroadcastCombatEvent(area, combatEventSpecialParry, actor.RuntimeID, primary.RuntimeID, int16(parryAge),
				buildImpactResolutionMetaText(entry.ID))
			if recover := resolveStageAction(entry.ActionOverride, ability.ActionRecover, "Idle"); recover != "" {
				BroadcastAnimate(area, actor, recover)
			}
			return false
		}
		primary.Mu.Unlock()
	}

	if impact := resolveStageAction(entry.ActionOverride, ability.ActionImpact, "Attack"); impact != "" {
		BroadcastAnimate(area, actor, impact)
	}

	radius := ability.TelegraphRadius
	if radius <= 0 {
		radius = 1.45 // matches buildSpecialWindupMetaText's own fallback
	}

	// Primary target (if any): dodge/parry above already gated on THIS
	// actor specifically — those are precise, personally-timed reactive
	// windows tied to being the telegraphed target, and stay scoped to the
	// primary target only. Resolved BEFORE the AoE loop so the loop can
	// skip it by RuntimeID.
	if primary != nil {
		if resolveSpecialImpactOnVictim(area, actor, primary, ability, entry.ID, ability.ID, now) {
			killedTarget = true
		}
	}

	// AoE splash (circle only for now — cone/line need angle/line math not
	// present anywhere in this codebase yet; documented as a follow-up, see
	// docs/TECH_DEBT.md). Uses entry.ImpactX/Z, captured/chosen ONCE when
	// this entry was created — NOT any actor's possibly-since-moved live
	// position. This is what makes the ground telegraph and the actual
	// damage area agree exactly.
	for _, victim := range ActorsInRadius(area, entry.ImpactX, entry.ImpactZ, radius) {
		if victim == nil || victim == actor {
			continue // caster never hits itself
		}
		if primary != nil && victim.RuntimeID == primary.RuntimeID {
			continue // primary target already resolved above
		}
		if victim.IsDead() || !IsHostileTo(actor, victim) {
			continue
		}
		if resolveSpecialImpactOnVictim(area, actor, victim, ability, entry.ID, ability.ID, now) {
			killedTarget = true
		}
	}

	return killedTarget
}

// resolveSpecialImpactOnVictim applies one special-ability hit to one
// victim: damage roll, guard mitigation, ApplyDamage, floating number +
// combat event, blood FX, HP broadcast, ability FX, skill tracking, and
// death handling. Shared by resolvePendingImpactEntry's primary-target
// resolution and its AoE splash loop so both hit exactly the same way — the
// only difference between them is that dodge/parry are checked by the
// caller BEFORE calling this, and only for the primary target.
// impactID travels in the hit/crit combat events' meta text (impact_id=N,
// same additive key=value pattern as buildSpecialWindupMetaText's
// impact_x/z) so the client knows WHICH of possibly several concurrent
// ground telegraphs from this source_rid to remove.
func resolveSpecialImpactOnVictim(area *Area, actor, victim *Actor, ability AbilityTemplate, impactID uint64, abilityID int, now int64) (justDied bool) {
	damage, isCrit := specialAttackDamage(actor, victim, ability)
	meta := buildImpactResolutionMetaText(impactID)

	// Guard: reduce ability damage if the victim is defending (mirrors ProcessAttack).
	victim.Mu.Lock()
	if victim.Guarding && victim.Stamina > 0 {
		damage = (damage*guardDamagePct + 99) / 100 // ceil(damage * pct / 100)
		if damage < 1 {
			damage = 1
		}
		victim.Stamina -= guardHitSPCost
		guardEnded := false
		if victim.Stamina <= 0 {
			victim.Stamina = 0
			victim.Guarding = false
			victim.GuardUntil = 0
			guardEnded = true
		}
		victim.Mu.Unlock()

		BroadcastCombatEvent(area, combatEventHitGuarded, actor.RuntimeID, victim.RuntimeID, int16(damage), meta)
		if guardEnded {
			BroadcastCombatEvent(area, combatEventGuardEnded, victim.RuntimeID, victim.RuntimeID, 0, "")
		}
	} else {
		victim.Mu.Unlock()
	}

	hp, justDied := ApplyDamage(victim, damage, actor.RuntimeID)
	if isCrit {
		BroadcastFloatingNumber(area, victim, int16(damage), 1)
		BroadcastCombatEvent(area, combatEventSpecialCritHit, actor.RuntimeID, victim.RuntimeID, int16(damage), meta)
	} else {
		BroadcastFloatingNumber(area, victim, int16(damage), 0)
		BroadcastCombatEvent(area, combatEventSpecialHit, actor.RuntimeID, victim.RuntimeID, int16(damage), meta)
	}
	if damage > 0 && victim.IsNPC && GetBloodMode() == "all" {
		if bloodFX := GetBloodFX(); bloodFX != "" {
			BroadcastBloodFX(area, actor, victim, bloodFX)
		}
	}
	BroadcastHPUpdate(area, victim, hp)
	BroadcastAbilityFX(area, actor, victim, ability, FXPhaseImpact)
	if !actor.IsNPC && actor.CharacterID != "" && victim.IsNPC {
		GetCombatWindowManager().TrackSkill(
			actor.RuntimeID,
			victim.RuntimeID,
			uint32(abilityID),
			int32(victim.Level),
			actor.CharacterID,
		)
	}
	if justDied {
		BroadcastAnimate(area, victim, "Death")
		BroadcastActorDead(area, victim.RuntimeID, actor.RuntimeID)
		OnNPCKilled(area, victim, actor.RuntimeID)
		runSpecialKillHook(area, actor, victim)
	} else {
		// CC/stat-mod buff-debuff (Buffs/Debuffs/CC, Fase 1+2) — never
		// applied to a target that just died from this same hit (no point
		// stunning/debuffing a corpse). Dispatches on the template's Kind —
		// see TryApplyStatusEffect (status_effects.go).
		TryApplyStatusEffect(area, actor, victim, ability, now)
	}
	return justDied
}

// IsHostileTo mirrors the only faction model this combat system has today:
// NPCs are hostile to players and vice versa (see area.go's tickAI, which
// splits area.actors into npcs/players purely on the IsNPC bool — no
// parties/factions/pets exist yet). Extracted here as its own helper (did
// not exist before — every prior combat path only ever had ONE fixed
// attacker/target pair, so hostility was implicit) because AoE splash needs
// to test it against an arbitrary list of nearby actors.
func IsHostileTo(a, b *Actor) bool {
	if a == nil || b == nil {
		return false
	}
	return a.IsNPC != b.IsNPC
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

	// 1) Resolve any pending impacts (shared flow for any actor).
	if handled, killed := ResolveActorPendingImpacts(area, npc, now); handled {
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

	// Impact center captured ONCE here, at windup start, from the target's
	// CURRENT position — never recomputed at resolution time (see
	// PendingImpact.ImpactX/Z doc, actor.go). This is what both the AoE
	// damage radius (resolvePendingImpactEntry) and the client's ground
	// telegraph use as their single source of truth.
	target.Mu.Lock()
	impactX := target.X
	impactZ := target.Z
	target.Mu.Unlock()

	impactID := NewImpactID()

	npc.Mu.Lock()
	previousLastSpecialAt := npc.LastSpecialAt
	if npc.AbilityCooldowns == nil {
		npc.AbilityCooldowns = make(map[int]int64)
	}
	markNPCSpecialChainCastStarted(npc, now, previousLastSpecialAt)
	npc.PendingImpacts = append(npc.PendingImpacts, PendingImpact{
		ID:                impactID,
		ResolveAt:         now + windupMs,
		TargetRID:         target.RuntimeID,
		ImpactX:           impactX,
		ImpactZ:           impactZ,
		Ability:           ability,
		ActionOverride:    actionOverride,
		ReasonTag:         reasonTag,
		ClientTraceID:     clientTraceID,
		GatesAbilityCasts: true, // classic single-target path — blocks a second cast while charging, same as the old scalar SpecialWindupUntil did
	})
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
		buildSpecialWindupMetaText(ability, reasonTag, clientTraceID, impactX, impactZ, impactID),
	)
	if windupAction := resolveStageAction(actionOverride, ability.ActionWindup, "Attack"); windupAction != "" {
		BroadcastAnimate(area, npc, windupAction)
	}
	BroadcastAbilityFX(area, npc, target, ability, FXPhaseWindup)
	return true
}

// SpawnScriptedImpact schedules ONE "raw" impact (telegraph + AoE damage,
// no personal target) at a script-chosen position and delay — the engine
// primitive behind NPCCombat.spawn_impact (scripting/api.go). Reuses the
// EXACT SAME resolution pipeline as the classic single-target special path
// (resolvePendingImpactEntry, ActorsInRadius, IsHostileTo,
// resolveSpecialImpactOnVictim) — the only difference is TargetRID=0 (no
// dodge/parry/guard personal-target checks — a mere AoE bystander doesn't
// get those) and GatesAbilityCasts=false (never blocks the caster's normal
// ability rotation — see canActorStartAbilityNow, cast_intent.go).
//
// A "meteor shower" is nothing more than a Lua script calling this several
// times with script-chosen positions/delays — there is no fixed
// "impact_count"/"stagger"/"spread" concept anywhere in the engine; the
// PATTERN is entirely the script's decision. See docs/TECH_DEBT.md, mob AoE
// + multi-impact investigation.
//
// Returns (impactID, true) on success, (0, false) if npcRID doesn't resolve
// to a live actor in some area.
func SpawnScriptedImpact(
	w *World,
	casterRID uint32,
	x, z float32,
	radius float32,
	damageMin, damageMax int32,
	delayMs int64,
	telegraphColorRGBA, telegraphStyle, vfxPathImpact string,
	actionOverride, reasonTag string,
	now int64,
) (uint64, bool) {
	if w == nil || casterRID == 0 {
		return 0, false
	}
	caster, area := w.FindActor(casterRID)
	if caster == nil || area == nil || caster.IsDead() {
		return 0, false
	}
	if delayMs < 0 {
		delayMs = 0
	}
	if radius <= 0 {
		radius = 1.45
	}
	if reasonTag == "" {
		reasonTag = "script_spawn_impact"
	}

	ability := AbilityTemplate{
		Name:            "scripted_spawn_impact",
		TelegraphType:   telegraphStyle,
		TelegraphRadius: radius,
		TelegraphColorRGBA: telegraphColorRGBA,
		BaseDamageMin:   damageMin,
		BaseDamageMax:   damageMax,
		ActionWindup:    "Attack",
		ActionImpact:    "Attack",
		ActionRecover:   "Idle",
		VFXPathImpact:   vfxPathImpact,
		Enabled:         true,
	}

	impactID := NewImpactID()

	caster.Mu.Lock()
	caster.PendingImpacts = append(caster.PendingImpacts, PendingImpact{
		ID:                impactID,
		ResolveAt:         now + delayMs,
		TargetRID:         0, // raw impact — no personal target, no dodge/parry/guard checks
		ImpactX:           x,
		ImpactZ:           z,
		Ability:           ability,
		ActionOverride:    actionOverride,
		ReasonTag:         reasonTag,
		GatesAbilityCasts: false, // never blocks the caster's normal ability rotation
	})
	caster.Mu.Unlock()

	BroadcastCombatEvent(
		area,
		combatEventSpecialWindup,
		caster.RuntimeID,
		0, // no personal target
		int16(delayMs),
		buildSpecialWindupMetaText(ability, reasonTag, "", x, z, impactID),
	)
	if windupAction := resolveStageAction(actionOverride, ability.ActionWindup, "Attack"); windupAction != "" {
		BroadcastAnimate(area, caster, windupAction)
	}
	BroadcastAbilityFX(area, caster, nil, ability, FXPhaseWindup)
	return impactID, true
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

	dim := resolveAbilityDimension(npc, ability)
	stats := selectDimensionStats(npcDerived, targetDerived, dim)

	baseMin := ability.BaseDamageMin
	baseMax := ability.BaseDamageMax
	if baseMin <= 0 && baseMax <= 0 {
		baseMin = stats.DmgMin
		baseMax = stats.DmgMax
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
	base += npcDerived.BonusDamageFlat

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
	defPct := ValueToPercent(stats.DefenseValue, defenseCap, defenseSoftcap)
	dmg = int32(float32(dmg) * (1.0 - defPct))
	if dmg < 1 {
		dmg = 1
	}

	// SkillDamageBoostPct applies before crit so it amplifies crit damage too.
	if npcDerived.SkillDamageBoostPct > 0 {
		dmg = int32(float32(dmg) * (1.0 + npcDerived.SkillDamageBoostPct))
		if dmg < 1 {
			dmg = 1
		}
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
		critPct = ValueToPercent(stats.CritValue, critValueCap, critValueSoftcap)
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

// buildImpactResolutionMetaText carries impact_id on the RESOLUTION events
// (dodge/parry/hit/critHit) — the counterpart to buildSpecialWindupMetaText,
// which carries it (plus impact_x/z) on the WINDUP event. Same additive
// "meta:key=value;..." format, parsed client-side by the same
// ParseCombatTelegraphMeta. Without this, a client that has multiple
// concurrent telegraphs from the same source_rid (a boss stacking several
// NPCCombat.spawn_impact calls) would have no way to know WHICH one just
// resolved and should stop drawing.
func buildImpactResolutionMetaText(impactID uint64) string {
	return fmt.Sprintf("meta:impact_id=%d", impactID)
}

func buildSpecialWindupMetaText(ability AbilityTemplate, reasonTag, clientTraceID string, impactX, impactZ float32, impactID uint64) string {
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
	// impact_x/impact_z: world-space ground telegraph center (see
	// PendingImpact.ImpactX/Z doc, actor.go). impact_id: which
	// PendingImpact this is — lets the client tell apart multiple
	// concurrent telegraphs from the same source_rid (see
	// buildImpactResolutionMetaText, used on the matching resolution
	// event). Both appended at the end so older clients parsing this same
	// "meta:" string with a key=value scanner (ParseCombatTelegraphMeta,
	// main.cpp) simply ignore unknown keys rather than breaking.
	trace := sanitizeCombatMetaValue(clientTraceID)
	if trace == "" {
		return fmt.Sprintf(
			"meta:telegraph=parry;ability=%d;reason=%s;radius=%.2f;color=%s;style=%s;window_ms=%d;impact_x=%.3f;impact_z=%.3f;impact_id=%d",
			ability.ID, reason, radius, color, style, parryWindowMs, impactX, impactZ, impactID,
		)
	}
	return fmt.Sprintf(
		"meta:telegraph=parry;ability=%d;reason=%s;radius=%.2f;color=%s;style=%s;window_ms=%d;trace=%s;impact_x=%.3f;impact_z=%.3f;impact_id=%d",
		ability.ID, reason, radius, color, style, parryWindowMs, trace, impactX, impactZ, impactID,
	)
}
