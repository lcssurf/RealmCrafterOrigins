// Package world - combat_ai_loadout.go
//
// NPC loadout decision and condition-evaluation helpers.
package world

import (
	"math"
	"math/rand"
	"strconv"
	"strings"
)

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

