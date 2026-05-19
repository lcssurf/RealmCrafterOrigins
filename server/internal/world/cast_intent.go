package world

import (
	"encoding/json"
	"strings"
	"time"
)

// CastIntent is the unified cast request contract used by script/input layers.
type CastIntent struct {
	CasterRID      uint32
	TargetRID      uint32
	AbilityID      int
	ActionOverride string
	ReasonTag      string
	ClientTraceID  string
}

const (
	castIntentCasterAny    = 0
	castIntentCasterNPC    = 1
	castIntentCasterPlayer = 2
)

// CanNPCCastByRID validates whether an NPC cast intent is currently eligible.
func CanNPCCastByRID(w *World, intent CastIntent) (bool, string) {
	return canCastByRIDAt(w, intent, time.Now().UnixMilli(), castIntentCasterNPC)
}

// TryStartNPCCastByRID validates and starts an NPC cast windup.
// Returns (started, reason). On success reason is "ok".
func TryStartNPCCastByRID(w *World, intent CastIntent) (bool, string) {
	return tryStartCastByRIDAt(w, intent, time.Now().UnixMilli(), castIntentCasterNPC)
}

// CanPlayerCastByRID validates whether a player cast intent is currently eligible.
func CanPlayerCastByRID(w *World, intent CastIntent) (bool, string) {
	return canCastByRIDAt(w, intent, time.Now().UnixMilli(), castIntentCasterPlayer)
}

// TryStartPlayerCastByRID validates and starts a player cast windup using the
// same authoritative ability runtime pipeline as NPC casts.
func TryStartPlayerCastByRID(w *World, intent CastIntent) (bool, string) {
	return tryStartCastByRIDAt(w, intent, time.Now().UnixMilli(), castIntentCasterPlayer)
}

func canCastByRIDAt(w *World, intent CastIntent, now int64, expectedCasterKind int) (bool, string) {
	caster, area, target, ability, _, reason := resolveCastIntentContext(w, intent, now, false, expectedCasterKind)
	if reason != "ok" {
		return false, reason
	}
	if caster == nil || area == nil || target == nil {
		return false, "invalid_intent_context"
	}
	if ability.ID <= 0 && intent.AbilityID > 0 {
		return false, "ability_not_found"
	}
	return true, "ok"
}

func tryStartCastByRIDAt(w *World, intent CastIntent, now int64, expectedCasterKind int) (bool, string) {
	finish := func(started bool, reason string) (bool, string) {
		recordCastIntentTelemetry(intent, expectedCasterKind, started, reason, now)
		return started, reason
	}

	caster, area, target, ability, override, reason := resolveCastIntentContext(w, intent, now, true, expectedCasterKind)
	if reason != "ok" {
		return finish(false, reason)
	}
	if caster == nil || area == nil || target == nil {
		return finish(false, "invalid_intent_context")
	}

	if ok, why := consumeCastResource(caster, ability); !ok {
		return finish(false, why)
	}
	if !startNPCSpecialCast(area, caster, target, ability, override, intent.ReasonTag, intent.ClientTraceID, now) {
		return finish(false, "start_failed")
	}
	if !caster.IsNPC {
		broadcastCastResourceUpdate(caster, ability)
	}
	return finish(true, "ok")
}

func resolveCastIntentContext(
	w *World,
	intent CastIntent,
	now int64,
	includeOverride bool,
	expectedCasterKind int,
) (caster *Actor, area *Area, target *Actor, ability AbilityTemplate, override string, reason string) {
	if w == nil {
		return nil, nil, nil, AbilityTemplate{}, "", "world_nil"
	}
	if intent.CasterRID == 0 || intent.TargetRID == 0 || intent.AbilityID <= 0 {
		return nil, nil, nil, AbilityTemplate{}, "", "invalid_payload"
	}

	caster, area = w.FindActor(intent.CasterRID)
	if caster == nil || area == nil {
		return nil, nil, nil, AbilityTemplate{}, "", "caster_not_found"
	}
	if caster.IsDead() {
		return nil, nil, nil, AbilityTemplate{}, "", "caster_dead"
	}
	if expectedCasterKind == castIntentCasterNPC && !caster.IsNPC {
		return nil, nil, nil, AbilityTemplate{}, "", "caster_not_npc"
	}
	if expectedCasterKind == castIntentCasterPlayer && caster.IsNPC {
		return nil, nil, nil, AbilityTemplate{}, "", "caster_not_player"
	}

	t, ok := area.GetActor(intent.TargetRID)
	if !ok || t == nil {
		return nil, nil, nil, AbilityTemplate{}, "", "target_not_found"
	}
	if t.IsDead() {
		return nil, nil, nil, AbilityTemplate{}, "", "target_dead"
	}
	target = t

	startReason := canActorStartAbilityNow(caster, target, intent.AbilityID, now)
	if startReason != "ok" {
		return nil, nil, nil, AbilityTemplate{}, "", startReason
	}

	ability, _ = resolveAbilityTemplate(intent.AbilityID)
	if !ability.Enabled {
		return nil, nil, nil, AbilityTemplate{}, "", "ability_disabled"
	}

	if includeOverride {
		override = resolveIntentActionOverride(caster, ability, intent.ActionOverride)
	}
	return caster, area, target, ability, override, "ok"
}

func canActorStartAbilityNow(caster, target *Actor, abilityID int, now int64) string {
	if abilityID <= 0 {
		return "ability_invalid"
	}
	ability, ok := resolveAbilityTemplate(abilityID)
	if !ok {
		return "ability_not_found"
	}
	if !ability.Enabled {
		return "ability_disabled"
	}

	caster.Mu.Lock()
	activeWindup := caster.SpecialWindupUntil > now
	lastSpecialAt := caster.LastSpecialAt
	if caster.AbilityCooldowns == nil {
		caster.AbilityCooldowns = make(map[int]int64)
	}
	caster.Mu.Unlock()
	if activeWindup {
		return "ability_in_windup"
	}
	globalGCDMs := resolveNPCGlobalGCDMs(caster)
	if globalGCDMs < 0 {
		globalGCDMs = 0
	}
	if now-lastSpecialAt < globalGCDMs {
		return "global_gcd"
	}
	if abilityOnCooldown(caster, ability, now) {
		return "ability_cooldown"
	}
	if !inSpecialRange(caster, target, ability.RangeMin, ability.RangeMax) {
		return "out_of_range"
	}
	if !hasCastResource(caster, ability) {
		return "resource_insufficient"
	}
	return "ok"
}

func hasCastResource(npc *Actor, ability AbilityTemplate) bool {
	cost := ability.ResourceCost
	if cost <= 0 {
		return true
	}
	npc.Mu.Lock()
	defer npc.Mu.Unlock()
	switch strings.ToLower(strings.TrimSpace(ability.ResourceType)) {
	case "mp", "ep", "energy":
		return npc.Energy >= cost
	case "sp", "stamina":
		return npc.Stamina >= cost
	default:
		return true
	}
}

func consumeCastResource(npc *Actor, ability AbilityTemplate) (bool, string) {
	cost := ability.ResourceCost
	if cost <= 0 {
		return true, "ok"
	}
	resourceType := strings.ToLower(strings.TrimSpace(ability.ResourceType))
	npc.Mu.Lock()
	defer npc.Mu.Unlock()
	switch resourceType {
	case "mp", "ep", "energy":
		if npc.Energy < cost {
			return false, "resource_insufficient"
		}
		npc.Energy -= cost
		return true, "ok"
	case "sp", "stamina":
		if npc.Stamina < cost {
			return false, "resource_insufficient"
		}
		npc.Stamina -= cost
		return true, "ok"
	default:
		return true, "ok"
	}
}

func broadcastCastResourceUpdate(caster *Actor, ability AbilityTemplate) {
	if caster == nil || caster.IsNPC {
		return
	}
	resourceType := strings.ToLower(strings.TrimSpace(ability.ResourceType))
	caster.Mu.Lock()
	mp := caster.Energy
	sp := caster.Stamina
	caster.Mu.Unlock()
	switch resourceType {
	case "mp", "ep", "energy":
		BroadcastMPUpdate(caster, mp)
	case "sp", "stamina":
		BroadcastSPUpdate(caster, sp)
	}
}

func resolveIntentActionOverride(caster *Actor, ability AbilityTemplate, actionOverride string) string {
	action := strings.TrimSpace(actionOverride)
	if action == "" || !ability.AllowActionOverride {
		return ""
	}
	if !actorHasAction(caster, action) {
		return ""
	}
	tags := parseAllowedActionTags(ability.AllowedActionTagsJSON)
	if len(tags) == 0 {
		return action
	}
	lowerAction := strings.ToLower(action)
	for _, tag := range tags {
		if tag == "" {
			continue
		}
		if lowerAction == tag || strings.Contains(lowerAction, tag) {
			return action
		}
	}
	return ""
}

func parseAllowedActionTags(raw string) []string {
	s := strings.TrimSpace(raw)
	if s == "" {
		return nil
	}
	var out []string
	if strings.HasPrefix(s, "[") {
		var arr []string
		if err := json.Unmarshal([]byte(s), &arr); err == nil {
			for _, t := range arr {
				tt := strings.ToLower(strings.TrimSpace(t))
				if tt != "" {
					out = append(out, tt)
				}
			}
			return out
		}
	}
	parts := strings.Split(s, ",")
	for _, p := range parts {
		pp := strings.ToLower(strings.TrimSpace(p))
		if pp != "" {
			out = append(out, pp)
		}
	}
	return out
}

func actorHasAction(actor *Actor, actionName string) bool {
	if actor == nil || actor.Appearance == nil || actionName == "" {
		return false
	}
	for _, anim := range actor.Appearance.Anims {
		if anim.Action == actionName {
			return true
		}
	}
	return false
}
