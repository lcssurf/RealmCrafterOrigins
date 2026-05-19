package world

import (
	"fmt"
	"math"
	"math/rand"
	"strconv"
	"strings"
	"time"
)

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

// Combat event codes mirrored from protocol (avoids circular import).
const (
	combatEventHitDodged     uint8 = 8
	combatEventHitGuarded    uint8 = 9
	combatEventHitParried    uint8 = 10
	combatEventSpecialWindup uint8 = 11
	combatEventSpecialParry  uint8 = 12
	combatEventSpecialHit    uint8 = 13
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

// InMeleeRange returns true if a1 is close enough to hit a2.
// Uses a1.AttackRange if set; falls back to the MeleeRange constant.
func InMeleeRange(a1, a2 *Actor) bool {
	dx := a1.X - a2.X
	dz := a1.Z - a2.Z
	dy := (a1.Y - a2.Y) / 5.0
	distSq := dx*dx + dz*dz + dy*dy
	base := a1.AttackRange
	if base == 0 {
		base = MeleeRange
	}
	max := base + a1.Radius + a2.Radius
	return distSq <= max*max
}

// ProcessAttack executes one melee attack from attacker -> target.
// Returns (damage, isCrit, onCooldown, result).
// damage == -1 means miss or fully avoided hit.
func ProcessAttack(attacker, target *Actor) (damage int32, isCrit bool, onCooldown bool, result AttackResult) {
	result = AttackResultNormal

	// Enforce attack cooldown under attacker lock.
	attacker.Mu.Lock()
	now := time.Now().UnixMilli()
	if now-attacker.LastAttack < CombatDelay {
		attacker.Mu.Unlock()
		return 0, false, true, result
	}
	attacker.LastAttack = now
	attacker.LastCombatAt = now
	attacker.Mu.Unlock()

	// 90% hit chance (RC formula 1).
	if rand.Intn(100) < 10 {
		return -1, false, false, AttackResultMiss
	}

	// Base damage from weapon + strength modifier.
	strength := attacker.Strength
	wdmg := attacker.WeaponDamage
	var dmg int32
	if wdmg == 0 {
		// Unarmed: fist damage from strength.
		dmg = strength/8 + rand.Int31n(11) - 5
	} else {
		dmg = wdmg
		if strength < wdmg {
			dmg -= rand.Int31n(4) + 5
		} else if strength > wdmg {
			dmg += rand.Int31n(4) + 5
		} else {
			dmg += rand.Int31n(11) - 5
		}
	}

	// Critical hit: 10% chance, doubles damage.
	if rand.Intn(10) == 0 {
		dmg *= 2
		isCrit = true
	}

	// Armor reduction (target's cached armor sum).
	dmg -= target.CachedArmor
	if dmg < 1 {
		dmg = 1
	}

	// Defensive reactions and damage application under target lock.
	now2 := time.Now().UnixMilli()
	target.Mu.Lock()
	defer target.Mu.Unlock()
	if target.Guarding && target.GuardUntil > 0 && now2 >= target.GuardUntil {
		target.Guarding = false
		target.GuardUntil = 0
	}
	if target.DodgeUntil > now2 {
		return -1, false, false, AttackResultDodged
	}
	if target.ParryUntil > now2 {
		target.ParryUntil = 0
		return -1, false, false, AttackResultParried
	}
	if target.Guarding && target.Stamina > 0 {
		dmg = (dmg*guardDamagePct + 99) / 100 // ceil(dmg * pct / 100)
		if dmg < 1 {
			dmg = 1
		}
		target.Stamina -= guardHitSPCost
		if target.Stamina <= 0 {
			target.Stamina = 0
			target.Guarding = false
			target.GuardUntil = 0
		}
		result = AttackResultGuarded
	}
	target.Health -= dmg

	return dmg, isCrit, false, result
}

// BroadcastAttack sends all combat-related packets for one hit:
//   - "H" (hit) to attacker if it's a player.
//   - "Y" (you were hit) to target if it's a player.
//   - "O" (observer) to all other players in the area.
//   - PFloatingNumber to all players in the area.
//   - PStatUpdate to the target if it's a player.
//   - PActorDead to all if the target died.
//
// Returns true if the target died.
func BroadcastAttack(area *Area, attacker, target *Actor, damage int32, isCrit bool, result AttackResult) bool {
	dmgType := uint8(0) // physical

	BroadcastAnimate(area, attacker, "Attack")

	// "H" packet -> attacker (if player).
	if !attacker.IsNPC {
		var p pb
		p.u8('H')
		p.u32(target.RuntimeID)
		p.u16(uint16(damage + 1)) // +1 so 0 = miss (RC convention)
		p.u8(dmgType)
		p.u8(boolU8(isCrit))
		attacker.Send(buildFrame(pAttackActor, p))
	}

	// "Y" packet -> target (if player).
	if !target.IsNPC {
		var p pb
		p.u8('Y')
		p.u32(attacker.RuntimeID)
		p.u16(uint16(damage + 1))
		p.u8(dmgType)
		p.u8(boolU8(isCrit))
		target.Send(buildFrame(pAttackActor, p))
	}

	// "O" packet -> all other players.
	{
		var p pb
		p.u8('O')
		p.u32(attacker.RuntimeID)
		p.u32(target.RuntimeID)
		frame := buildFrame(pAttackActor, p)
		area.Mu.RLock()
		for _, a := range area.actors {
			if a.IsNPC || a == attacker || a == target {
				continue
			}
			a.Send(frame)
		}
		area.Mu.RUnlock()
	}

	// PFloatingNumber -> all players.
	{
		var p pb
		p.u32(target.RuntimeID)
		if damage == -1 {
			p.i16(-1) // miss
		} else {
			p.i16(int16(damage))
		}
		p.u8(boolU8(isCrit))
		frame := buildFrame(pFloatingNum, p)
		area.Mu.RLock()
		for _, a := range area.actors {
			if !a.IsNPC {
				a.Send(frame)
			}
		}
		area.Mu.RUnlock()
	}

	switch result {
	case AttackResultDodged:
		BroadcastCombatEvent(area, combatEventHitDodged, target.RuntimeID, attacker.RuntimeID, 0, "")
	case AttackResultGuarded:
		BroadcastCombatEvent(area, combatEventHitGuarded, target.RuntimeID, attacker.RuntimeID, int16(damage), "")
	case AttackResultParried:
		BroadcastCombatEvent(area, combatEventHitParried, target.RuntimeID, attacker.RuntimeID, 0, "")
	}

	// Broadcast "Hit" animation on the target if it has that action.
	// Only when damage was actually dealt (not a miss).
	if damage > 0 {
		BroadcastAnimate(area, target, "Hit")
	}

	// PStatUpdate -> target if it's a player.
	target.Mu.Lock()
	hp := target.Health
	sp := target.Stamina
	dead := hp <= 0 && target.DeadAt == 0
	now2 := time.Now().UnixMilli()
	if dead {
		target.DeadAt = now2
	}
	target.LastCombatAt = now2
	target.Mu.Unlock()

	{
		var p pb
		p.u8('A')
		p.u32(target.RuntimeID)
		p.u8(0) // attr 0 = HP
		p.i16(int16(hp))
		frame := buildFrame(pStatUpdate, p)
		if target.IsNPC {
			// Broadcast NPC HP to all players so their target bars update.
			area.Mu.RLock()
			for _, a := range area.actors {
				if !a.IsNPC {
					a.Send(frame)
				}
			}
			area.Mu.RUnlock()
		} else {
			target.Send(frame)
		}
	}
	if !target.IsNPC && result == AttackResultGuarded {
		BroadcastSPUpdate(target, sp)
	}

	if dead {
		BroadcastAnimate(area, target, "Death")
		// PActorDead -> all players.
		var p pb
		p.u32(target.RuntimeID)
		p.u32(attacker.RuntimeID)
		frame := buildFrame(pActorDead, p)
		area.Mu.RLock()
		for _, a := range area.actors {
			if !a.IsNPC {
				a.Send(frame)
			}
		}
		area.Mu.RUnlock()
	}

	return dead
}

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
				return true, false
			}
			target = forced
		}
	}
	if target == nil || target.IsDead() {
		return true, false
	}
	if !inSpecialRange(actor, target, ability.RangeMin, ability.RangeMax) {
		// Target escaped the special impact radius.
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
	damage := specialAttackDamage(actor, target, ability)
	hp, justDied := ApplyDamage(target, damage, actor.RuntimeID)
	BroadcastFloatingNumber(area, target, int16(damage), 0)
	BroadcastHPUpdate(area, target, hp)
	BroadcastCombatEvent(area, combatEventSpecialHit, actor.RuntimeID, target.RuntimeID, int16(damage), "")
	if !actor.IsNPC && actor.CharacterID != "" {
		runSpecialHitHook(area, actor, target, windupAbilityID)
	}
	if justDied {
		BroadcastAnimate(area, target, "Death")
		BroadcastActorDead(area, target.RuntimeID, actor.RuntimeID)
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

func selectNPCSpecialIntent(npc, target *Actor, now, lastSpecialAt int64) (npcSpecialIntent, bool) {
	globalGCDMs := resolveNPCGlobalGCDMs(npc)
	if globalGCDMs < 0 {
		globalGCDMs = 0
	}
	if now-lastSpecialAt < globalGCDMs {
		return npcSpecialIntent{}, false
	}
	if !canNPCStartSpecialChain(npc, now) {
		return npcSpecialIntent{}, false
	}

	loadouts := resolveNPCAbilityLoadout(npc.SpawnID, npc.ActorDefID)
	if !abilityRuntimeIsEnabled() || len(loadouts) == 0 {
		// Legacy fallback keeps old gameplay while content is migrated to DB.
		if !inSpecialRange(npc, target, 0, 0) {
			return npcSpecialIntent{}, false
		}
		if now-lastSpecialAt < npcSpecialCooldownMsLegacy {
			return npcSpecialIntent{}, false
		}
		if rand.Intn(100) >= npcSpecialChancePctLegacy {
			return npcSpecialIntent{}, false
		}
		return npcSpecialIntent{Ability: legacySpecialAbilityTemplate()}, true
	}

	evalCtx := buildLoadoutEvalContext(npc, target)

	for _, slot := range loadouts {
		ability, ok := resolveAbilityTemplate(slot.AbilityID)
		if !ok || !ability.Enabled {
			continue
		}
		if !matchesLoadoutPhaseTag(slot.PhaseTag, evalCtx) {
			continue
		}
		if !evaluateConditionLua(slot.ConditionLua, evalCtx) {
			continue
		}
		if abilityOnCooldown(npc, ability, now) {
			continue
		}
		if !inSpecialRange(npc, target, resolveSpecialMinRange(slot, ability), resolveSpecialMaxRange(npc, slot, ability)) {
			continue
		}
		if !isTargetHPAllowed(target, slot.MinTargetHPPct, slot.MaxTargetHPPct) {
			continue
		}
		if !rollLoadoutWeight(slot.Weight) {
			continue
		}
		return npcSpecialIntent{Ability: ability}, true
	}
	return npcSpecialIntent{}, false
}

func buildLoadoutEvalContext(npc, target *Actor) loadoutEvalContext {
	ctx := loadoutEvalContext{
		phaseTag:   "phase_1",
		phaseIndex: 1,
	}
	if npc == nil || target == nil {
		return ctx
	}

	npc.Mu.Lock()
	nx, ny, nz := npc.X, npc.Y, npc.Z
	nhp, nhpMax := npc.Health, npc.HealthMax
	nsp, nspMax := npc.Stamina, npc.StaminaMax
	nmp, nmpMax := npc.Energy, npc.EnergyMax
	npc.Mu.Unlock()

	target.Mu.Lock()
	tx, ty, tz := target.X, target.Y, target.Z
	thp, thpMax := target.Health, target.HealthMax
	tsp, tspMax := target.Stamina, target.StaminaMax
	tmp, tmpMax := target.Energy, target.EnergyMax
	target.Mu.Unlock()

	dx := float64(nx - tx)
	dz := float64(nz - tz)
	dy := float64(ny-ty) / 5.0
	ctx.distance = float32(math.Sqrt(dx*dx + dz*dz + dy*dy))

	ctx.npcHPPct = pctOf(nhp, nhpMax)
	ctx.targetHPPct = pctOf(thp, thpMax)
	ctx.npcSPPct = pctOf(nsp, nspMax)
	ctx.targetSPPct = pctOf(tsp, tspMax)
	ctx.npcMPPct = pctOf(nmp, nmpMax)
	ctx.targetMPPct = pctOf(tmp, tmpMax)

	switch {
	case ctx.npcHPPct <= 33:
		ctx.phaseTag = "phase_3"
		ctx.phaseIndex = 3
	case ctx.npcHPPct <= 66:
		ctx.phaseTag = "phase_2"
		ctx.phaseIndex = 2
	default:
		ctx.phaseTag = "phase_1"
		ctx.phaseIndex = 1
	}
	return ctx
}

func pctOf(v, max int32) float32 {
	if max <= 0 {
		return 0
	}
	p := (float32(v) / float32(max)) * 100.0
	if p < 0 {
		return 0
	}
	if p > 100 {
		return 100
	}
	return p
}

func matchesLoadoutPhaseTag(rawTag string, ctx loadoutEvalContext) bool {
	tag := strings.TrimSpace(strings.ToLower(rawTag))
	if tag == "" || tag == "any" || tag == "*" {
		return true
	}
	parts := strings.Split(tag, ",")
	for _, p := range parts {
		item := strings.TrimSpace(strings.ToLower(p))
		switch item {
		case "phase_1", "phase_2", "phase_3":
			if item == ctx.phaseTag {
				return true
			}
		case "enrage":
			if ctx.npcHPPct <= 20 {
				return true
			}
		case "execute":
			if ctx.targetHPPct <= 30 {
				return true
			}
		}
	}
	return false
}

func evaluateConditionLua(raw string, ctx loadoutEvalContext) bool {
	expr := strings.TrimSpace(raw)
	if expr == "" {
		return true
	}
	expr = strings.ReplaceAll(expr, "\n", " ")
	expr = strings.ReplaceAll(expr, "\r", " ")
	expr = strings.ReplaceAll(expr, "\t", " ")
	expr = strings.ReplaceAll(expr, "&&", " and ")
	expr = strings.ReplaceAll(expr, "||", " or ")
	expr = strings.Join(strings.Fields(strings.ToLower(expr)), " ")
	if expr == "" {
		return true
	}

	orParts := strings.Split(expr, " or ")
	for _, orPart := range orParts {
		andParts := strings.Split(orPart, " and ")
		ok := true
		for _, clause := range andParts {
			if !evaluateConditionClause(strings.TrimSpace(clause), ctx) {
				ok = false
				break
			}
		}
		if ok {
			return true
		}
	}
	return false
}

func evaluateConditionClause(clause string, ctx loadoutEvalContext) bool {
	if clause == "" {
		return true
	}
	negated := false
	if strings.HasPrefix(clause, "not ") {
		negated = true
		clause = strings.TrimSpace(strings.TrimPrefix(clause, "not "))
	}
	if clause == "true" {
		return !negated
	}
	if clause == "false" {
		return negated
	}

	ops := []string{"<=", ">=", "==", "!=", "<", ">"}
	var op string
	var idx int
	for _, candidate := range ops {
		if i := strings.Index(clause, candidate); i >= 0 {
			op = candidate
			idx = i
			break
		}
	}
	if op == "" {
		// Unsupported clause syntax -> fail closed.
		return false
	}

	left := strings.TrimSpace(clause[:idx])
	right := strings.TrimSpace(clause[idx+len(op):])
	if left == "" || right == "" {
		return false
	}

	// String comparison for phase tag.
	if left == "phase_tag" {
		rightText := strings.Trim(right, "\"'")
		res := compareText(op, strings.ToLower(ctx.phaseTag), strings.ToLower(rightText))
		if negated {
			return !res
		}
		return res
	}

	lv, lok := resolveConditionNumericValue(left, ctx)
	rv, rok := resolveConditionNumericValue(right, ctx)
	if !lok || !rok {
		return false
	}
	res := compareNumeric(op, lv, rv)
	if negated {
		return !res
	}
	return res
}

func resolveConditionNumericValue(token string, ctx loadoutEvalContext) (float64, bool) {
	switch strings.TrimSpace(strings.ToLower(token)) {
	case "distance":
		return float64(ctx.distance), true
	case "npc_hp_pct":
		return float64(ctx.npcHPPct), true
	case "target_hp_pct":
		return float64(ctx.targetHPPct), true
	case "npc_sp_pct":
		return float64(ctx.npcSPPct), true
	case "target_sp_pct":
		return float64(ctx.targetSPPct), true
	case "npc_mp_pct":
		return float64(ctx.npcMPPct), true
	case "target_mp_pct":
		return float64(ctx.targetMPPct), true
	case "phase":
		return ctx.phaseIndex, true
	case "rand_pct":
		return rand.Float64() * 100.0, true
	}
	if v, err := strconv.ParseFloat(token, 64); err == nil {
		return v, true
	}
	return 0, false
}

func compareNumeric(op string, left, right float64) bool {
	switch op {
	case "<":
		return left < right
	case "<=":
		return left <= right
	case ">":
		return left > right
	case ">=":
		return left >= right
	case "==":
		return left == right
	case "!=":
		return left != right
	default:
		return false
	}
}

func compareText(op, left, right string) bool {
	switch op {
	case "==":
		return left == right
	case "!=":
		return left != right
	default:
		return false
	}
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

func rollLoadoutWeight(weight int) bool {
	switch {
	case weight <= 0:
		return false
	case weight >= 100:
		return true
	default:
		return rand.Intn(100) < weight
	}
}

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
	return true
}

func specialAttackDamage(npc, target *Actor, ability AbilityTemplate) int32 {
	npc.Mu.Lock()
	fallbackBase := npc.WeaponDamage*2 + npc.Strength/2 + int32(npc.Level)*2
	npc.Mu.Unlock()

	target.Mu.Lock()
	armor := target.CachedArmor
	hpMax := target.HealthMax
	target.Mu.Unlock()

	baseMin := ability.BaseDamageMin
	baseMax := ability.BaseDamageMax
	if baseMin > baseMax {
		baseMin, baseMax = baseMax, baseMin
	}
	var base int32
	if baseMin <= 0 && baseMax <= 0 {
		base = fallbackBase
	} else {
		base = baseMin
		if baseMax > baseMin {
			base += rand.Int31n(baseMax - baseMin + 1)
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
		return dmg
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
	return dmg
}

// BroadcastCombatEvent sends PCombatEvent to all players in the area.
// Payload: event_code(u8) + source_rid(u32) + target_rid(u32) + value(i16) + text(str)
func BroadcastCombatEvent(area *Area, eventCode uint8, sourceRID, targetRID uint32, value int16, text string) {
	var p pb
	p.u8(eventCode)
	p.u32(sourceRID)
	p.u32(targetRID)
	p.i16(value)
	p.str(text)
	frame := buildFrame(pCombatEvent, p)

	area.Mu.RLock()
	for _, a := range area.actors {
		if !a.IsNPC {
			a.Send(frame)
		}
	}
	area.Mu.RUnlock()
}

// BroadcastAnimate sends PAnimateActor{rid, action_id} to all players in the area.
// action_id is the 0-based index of the action in actor.Appearance.Anims.
// If the actor has no Appearance or the action is not found, the call is a no-op.
func BroadcastAnimate(area *Area, actor *Actor, actionName string) {
	if actor.Appearance == nil || len(actor.Appearance.Anims) == 0 {
		return
	}
	var actionID uint8 = 0xFF
	for i, anim := range actor.Appearance.Anims {
		if anim.Action == actionName {
			actionID = uint8(i)
			break
		}
	}
	if actionID == 0xFF {
		return
	}
	actor.Mu.Lock()
	actor.CurrentAction = actionName
	actor.Mu.Unlock()
	var p pb
	p.u32(actor.RuntimeID)
	p.u8(actionID)
	frame := buildFrame(pAnimateActor, p)
	area.Mu.RLock()
	for _, a := range area.actors {
		if !a.IsNPC {
			a.Send(frame)
		}
	}
	area.Mu.RUnlock()
}

func boolU8(v bool) uint8 {
	if v {
		return 1
	}
	return 0
}

func sanitizeCombatMetaValue(raw string) string {
	s := strings.TrimSpace(raw)
	if s == "" {
		return ""
	}
	replacer := strings.NewReplacer(";", "_", "=", "_", "\r", "_", "\n", "_")
	return replacer.Replace(s)
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
