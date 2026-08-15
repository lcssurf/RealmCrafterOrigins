// Package world - status_effects.go
//
// Fase 1 of the Buffs/Debuffs/CC system shipped Crowd Control only (stun/
// root/silence/slow). Fase 2 (this round) activates stat-modifier buffs/
// debuffs (StatMods + the 3 StackRule variants) on the SAME ActiveEffects
// model — DoT/HoT tick fields are still a future phase. See
// docs/TECH_DEBT.md, Buffs/Debuffs/CC investigation, for the full design.
package world

import (
	"encoding/json"
	"log"
	"math/rand"
	"sync"
)

// StatusEffectTemplate mirrors one row in status_effect_templates (GUE-
// authored, tools/gue/src/tabs/status_effects.h/.cpp). StatMods/MaxStacks
// are only meaningful when Kind==StatusKindBuff/StatusKindDebuff — ignored
// for Kind==StatusKindCC (CCType/slow-multiplier already cover that case).
type StatusEffectTemplate struct {
	ID         int
	Name       string
	Kind       StatusEffectKind
	CCType     CCType
	DurationMs int64
	StackRule  string
	IconPath   string
	Enabled    bool
	StatMods   []StatMod
	MaxStacks  int
	// TickIntervalMs/TickDamageMin/TickDamageMax/IsHeal — Fase 3 (DoT/HoT).
	// TickIntervalMs<=0 (the default) means no tick — every existing Fase
	// 1/2 row (CC + plain stat buffs/debuffs) is unaffected. Only
	// meaningful for Kind==StatusKindBuff (IsHeal=true, a HoT) or
	// Kind==StatusKindDebuff (IsHeal=false, a DoT) — ignored for
	// Kind==StatusKindCC.
	TickIntervalMs int64
	TickDamageMin  int32
	TickDamageMax  int32
	IsHeal         bool
}

// ParseStatModsJSON decodes a status_effect_templates.stat_mods_json cell
// (GUE-authored, e.g. `[{"stat":"movement_speed_mult","flat":0,"pct":0.2}]`)
// into []StatMod. Empty/invalid input returns nil, never an error — a
// malformed row should behave like "no stat mods configured", not crash
// catalog loading (same tolerance as the rest of this codebase's
// DB-row-to-runtime-struct conversions).
func ParseStatModsJSON(raw string) []StatMod {
	if raw == "" {
		return nil
	}
	var mods []StatMod
	if err := json.Unmarshal([]byte(raw), &mods); err != nil {
		return nil
	}
	return mods
}

var (
	statusEffectMu  sync.RWMutex
	statusEffectByID map[int]StatusEffectTemplate
)

// SetStatusEffectCatalog replaces the in-memory status_effect_templates catalog.
func SetStatusEffectCatalog(templates []StatusEffectTemplate) {
	next := make(map[int]StatusEffectTemplate, len(templates))
	for _, t := range templates {
		if t.ID <= 0 {
			continue
		}
		next[t.ID] = t
	}
	statusEffectMu.Lock()
	statusEffectByID = next
	statusEffectMu.Unlock()
}

func resolveStatusEffectTemplate(id int) (StatusEffectTemplate, bool) {
	statusEffectMu.RLock()
	defer statusEffectMu.RUnlock()
	t, ok := statusEffectByID[id]
	return t, ok
}

// StatusEffectHook is implemented by the net layer to serialize and
// broadcast PStatusEffectDelta — mirrors AbilityFXHook/BloodFXHook
// (combat_fx.go)'s "world package owns WHEN, net package owns HOW to
// encode" split. applied=true means the effect was just applied/refreshed;
// applied=false means it just expired (client should stop gating input /
// drop the visual for this (targetRID, ccType) pair).
//
// durationMs is RELATIVE (ms remaining from now), not an absolute
// timestamp — same reasoning as the client's existing SkillHotbar cooldown
// pattern (server sends a duration, client stamps its own local receipt
// time and counts down locally; see client/src/ui/skill_hotbar.h). This
// also sidesteps needing 64-bit wire fields for an epoch-ms timestamp — the
// Writer in this codebase only has 32-bit integer/float writers.
//
// effectID/TemplateID are NOT part of the wire payload — Fase 1's "refresh"
// stack rule means at most one CC of a given CCType is ever active per
// actor at a time, so (targetRID, ccType) is already a unique key
// client-side; a global effect id isn't needed until stacking (Fase 2+)
// requires disambiguating multiple concurrent instances of the same type.
type StatusEffectHook func(
	area *Area,
	targetRID uint32,
	templateID int,
	kind, ccType string,
	durationMs int64,
	iconPath string,
	applied bool,
)

var (
	statusEffectHookMu sync.RWMutex
	statusEffectHook    StatusEffectHook
)

// SetStatusEffectHook registers the net-layer callback that actually builds
// and sends PStatusEffectDelta. Mirrors SetAbilityFXHook/SetBloodFXHook.
func SetStatusEffectHook(h StatusEffectHook) {
	statusEffectHookMu.Lock()
	statusEffectHook = h
	statusEffectHookMu.Unlock()
}

// BroadcastStatusEffectDelta dispatches one apply/expire event for a status
// effect. Actual packet serialization/broadcast is owned by the net hook —
// this is what makes PStatusEffectDelta a REAL wire path instead of the
// reserved-but-dead packet ID it was before this round (confirmed dead:
// docs/TECH_DEBT.md, Buffs/Debuffs/CC investigation, item 1).
func BroadcastStatusEffectDelta(area *Area, targetRID uint32, effect ActiveStatusEffect, tmpl StatusEffectTemplate, applied bool, now int64) {
	statusEffectHookMu.RLock()
	h := statusEffectHook
	statusEffectHookMu.RUnlock()
	if h == nil || area == nil || targetRID == 0 {
		return
	}
	remainingMs := effect.ExpiresAt - now
	if remainingMs < 0 {
		remainingMs = 0
	}
	h(area, targetRID, effect.TemplateID,
		string(effect.Kind), string(effect.CCType),
		remainingMs, tmpl.IconPath, applied)
}

// TryApplyCC rolls and (on success) applies the CC status effect configured
// on ability (via ability.StatusEffectTemplateID) to victim. No-op if the
// ability has no status effect configured, the template doesn't resolve to
// a CC-kind effect, or the victim is dead/nil.
//
// Chance formula (see docs/TECH_DEBT.md, Buffs/Debuffs/CC investigation,
// item 3 — this deviates from a LITERAL "1 + raw_stat_value" reading of the
// approved design on purpose): CCChanceValue/CCResistanceValue are raw
// accumulated stat POINTS, not percentages (same shape as CritValue/
// DefenseValue — see attributes.go), so using them directly as
// "1+value" multipliers would be nonsensical (a value of 50 would be a 51x
// multiplier). Instead this reuses the SAME diminishing-returns conversion
// (ValueToPercent + ccValueSoftcap/ccValueCap, attributes.go:336-338 —
// already defined, never consumed before this) already established for
// crit/defense/evasion, which is the idiomatic way this codebase turns a
// raw stat into a percentage:
//
//	attackerBonusPct  = ValueToPercent(attacker.CCChanceValue,     ccValueCap, ccValueSoftcap)
//	defenderResistPct = ValueToPercent(victim.CCResistanceValue,   ccValueCap, ccValueSoftcap)
//	finalChance       = clamp01(ability.CCBaseChancePct/100 * (1+attackerBonusPct) * (1-defenderResistPct))
//
// Duration formula: base duration scaled by the ATTACKER's DebuffDurationPct
// (CC is a debuff from the victim's perspective, and the confirmed
// interpretation — see task context — is that BuffDurationPct/
// DebuffDurationPct are ATTACKER-side stats scaling what THEY apply):
//
//	finalDurationMs = base_duration_ms * (1 + attacker.DebuffDurationPct)
func TryApplyCC(area *Area, attacker, victim *Actor, ability AbilityTemplate, now int64) bool {
	if area == nil || attacker == nil || victim == nil {
		return false
	}
	if ability.StatusEffectTemplateID <= 0 {
		return false
	}
	tmpl, ok := resolveStatusEffectTemplate(ability.StatusEffectTemplateID)
	if !ok || !tmpl.Enabled || tmpl.Kind != StatusKindCC || tmpl.CCType == "" {
		return false
	}
	if victim.IsDead() {
		return false
	}

	attacker.Mu.Lock()
	attackerCCChance := attacker.Derived.CCChanceValue
	attackerDebuffDurationPct := attacker.Derived.DebuffDurationPct
	attacker.Mu.Unlock()

	victim.Mu.Lock()
	victimCCResist := victim.Derived.CCResistanceValue
	victim.Mu.Unlock()

	attackerBonusPct := ValueToPercent(attackerCCChance, ccValueCap, ccValueSoftcap)
	defenderResistPct := ValueToPercent(victimCCResist, ccValueCap, ccValueSoftcap)

	baseChance := ability.CCBaseChancePct / 100.0
	finalChance := baseChance * (1.0 + attackerBonusPct) * (1.0 - defenderResistPct)
	if finalChance < 0 {
		finalChance = 0
	}
	if finalChance > 1 {
		finalChance = 1
	}
	if rand.Float32() >= finalChance {
		return false // roll failed
	}

	durationMs := int64(float64(tmpl.DurationMs) * (1.0 + float64(attackerDebuffDurationPct)))
	if durationMs < 0 {
		durationMs = 0
	}

	effect := ActiveStatusEffect{
		ID:         NewImpactID(), // shared global counter — same pattern as PendingImpact.ID, no reason for a second counter
		TemplateID: tmpl.ID,
		Kind:       StatusKindCC,
		CCType:     tmpl.CCType,
		AppliedAt:  now,
		ExpiresAt:  now + durationMs,
		SourceRID:  attacker.RuntimeID,
		StackRule:  "refresh", // Fase 1: only rule implemented, see ActiveStatusEffect doc (actor.go)
	}

	victim.Mu.Lock()
	if i := victim.findActiveCCLocked(tmpl.CCType); i >= 0 {
		// "refresh" stack rule: replace in place, keep the same slot so a
		// re-application doesn't grow the slice unboundedly.
		victim.ActiveEffects[i] = effect
	} else {
		victim.ActiveEffects = append(victim.ActiveEffects, effect)
	}
	victim.Mu.Unlock()

	BroadcastStatusEffectDelta(area, victim.RuntimeID, effect, tmpl, true, now)
	return true
}

// TryApplyStatusEffect is the single generic entry point for applying
// WHATEVER status effect an ability's StatusEffectTemplateID points at —
// dispatches to TryApplyCC (Kind==StatusKindCC) or TryApplyStatMod
// (Kind==StatusKindBuff/StatusKindDebuff). combat_special.go's on-hit
// resolution calls this instead of TryApplyCC directly now, so an ability
// can deliver either kind through the exact same trigger point without the
// caller needing to know which. source==target is a valid, expected case
// (a self-buff — "aplicado a si mesmo"; see scripting/api.go's
// Actor.apply_status_effect for the Lua-callable ally/self path that also
// funnels through here).
func TryApplyStatusEffect(area *Area, source, target *Actor, ability AbilityTemplate, now int64) bool {
	if area == nil || source == nil || target == nil || ability.StatusEffectTemplateID <= 0 {
		return false
	}
	tmpl, ok := resolveStatusEffectTemplate(ability.StatusEffectTemplateID)
	if !ok || !tmpl.Enabled {
		return false
	}
	switch tmpl.Kind {
	case StatusKindCC:
		return TryApplyCC(area, source, target, ability, now)
	case StatusKindBuff, StatusKindDebuff:
		return TryApplyStatMod(area, source, target, ability, now)
	default:
		return false
	}
}

// TryApplyStatMod rolls and (on success) applies the stat-modifier buff/
// debuff configured on ability to target. Mirrors TryApplyCC's formula
// shape (see its doc comment for the full reasoning):
//
// Chance — a debuff applied to a DIFFERENT actor (the normal "landed a hit
// that also debuffs" case) rolls exactly like CC: attacker's CCChanceValue
// vs target's CCResistanceValue, converted via the same ValueToPercent
// softcap curve. A buff (or a debuff a script applies to itself, which
// shouldn't happen but is handled the same way defensively) applied to
// SELF never rolls resistance — self-application only rolls the ability's
// own base chance (ability.CCBaseChancePct/100; test data sets this to 100
// for an always-succeeds self-buff, same as any other ability wanting a
// guaranteed effect).
//
// Duration — scaled by the SOURCE's BuffDurationPct (Kind==Buff) or
// DebuffDurationPct (Kind==Debuff), same "duration bonus belongs to
// whoever applied it" rule Fase 1 established for CC.
//
// Stacking — delegates to Actor.applyStackRuleLocked (actor.go), which
// implements all 3 StackRule variants; a false return (ignore_if_active
// with one already active) short-circuits here before any broadcast/
// recompute, exactly like a failed chance roll.
func TryApplyStatMod(area *Area, source, target *Actor, ability AbilityTemplate, now int64) bool {
	if area == nil || source == nil || target == nil {
		return false
	}
	if ability.StatusEffectTemplateID <= 0 {
		return false
	}
	tmpl, ok := resolveStatusEffectTemplate(ability.StatusEffectTemplateID)
	if !ok || !tmpl.Enabled || (tmpl.Kind != StatusKindBuff && tmpl.Kind != StatusKindDebuff) {
		return false
	}
	if target.IsDead() {
		return false
	}

	selfApplied := source == target

	var finalChance float32
	if selfApplied {
		finalChance = ability.CCBaseChancePct / 100.0
	} else {
		source.Mu.Lock()
		srcChance := source.Derived.CCChanceValue
		source.Mu.Unlock()
		target.Mu.Lock()
		tgtResist := target.Derived.CCResistanceValue
		target.Mu.Unlock()

		attackerBonusPct := ValueToPercent(srcChance, ccValueCap, ccValueSoftcap)
		defenderResistPct := ValueToPercent(tgtResist, ccValueCap, ccValueSoftcap)
		finalChance = ability.CCBaseChancePct / 100.0 * (1.0 + attackerBonusPct) * (1.0 - defenderResistPct)
	}
	if finalChance < 0 {
		finalChance = 0
	}
	if finalChance > 1 {
		finalChance = 1
	}
	if rand.Float32() >= finalChance {
		return false // roll failed
	}

	source.Mu.Lock()
	var durationPct float32
	if tmpl.Kind == StatusKindDebuff {
		durationPct = source.Derived.DebuffDurationPct
	} else {
		durationPct = source.Derived.BuffDurationPct
	}
	source.Mu.Unlock()

	durationMs := int64(float64(tmpl.DurationMs) * (1.0 + float64(durationPct)))
	if durationMs < 0 {
		durationMs = 0
	}

	effect := ActiveStatusEffect{
		ID:                NewImpactID(),
		TemplateID:        tmpl.ID,
		Kind:              tmpl.Kind,
		AppliedAt:         now,
		ExpiresAt:         now + durationMs,
		SourceRID:         source.RuntimeID,
		StackRule:         tmpl.StackRule,
		StatMods:          tmpl.StatMods,
		TemplateMaxStacks: tmpl.MaxStacks,
		// Fase 3 (DoT/HoT) — LastTickAt starts at `now` (apply time) so the
		// first tick fires one full TickIntervalMs later, not immediately.
		TickIntervalMs: tmpl.TickIntervalMs,
		TickDamageMin:  tmpl.TickDamageMin,
		TickDamageMax:  tmpl.TickDamageMax,
		IsHeal:         tmpl.IsHeal,
		LastTickAt:     now,
	}

	target.Mu.Lock()
	applied := target.applyStackRuleLocked(effect)
	target.Mu.Unlock()
	// TEMP DIAG (Buffs/Debuffs/CC — "MovementSpeedMult não muda" investigation):
	// confirms the effect actually landed in ActiveEffects, with what
	// StatMods, and prints Derived.MovementSpeedMult before/after the
	// recompute below.
	log.Printf("[statmod-diag] TryApplyStatMod: target=%d tmpl=%d(%q) kind=%s stackRule=%s applied=%v statMods=%+v",
		target.RuntimeID, tmpl.ID, tmpl.Name, tmpl.Kind, tmpl.StackRule, applied, tmpl.StatMods)
	if !applied {
		return false // ignore_if_active dropped it
	}

	target.Mu.Lock()
	beforeMult := target.Derived.MovementSpeedMult
	target.Mu.Unlock()

	// Order matters: Derived MUST be fully recomputed (StatMods baked in)
	// BEFORE the broadcast/hook fires — BroadcastStatusEffectDelta is what
	// triggers handleStatusEffectBroadcast (net/status_effect_bridge.go),
	// which resends PFullStats reading target.Derived AT THAT MOMENT. The
	// previous order (broadcast, then recompute) sent the pre-buff snapshot
	// — confirmed by real log ("MovementSpeedMult não muda" investigation):
	// the resend showed the OLD value (1.0100) because RecomputeDerivedStatsFast
	// hadn't run yet when handleStatusEffectBroadcast read Derived.
	RecomputeDerivedStatsFast(target)

	target.Mu.Lock()
	afterMult := target.Derived.MovementSpeedMult
	target.Mu.Unlock()
	log.Printf("[statmod-diag] TryApplyStatMod: target=%d MovementSpeedMult before=%.4f after=%.4f",
		target.RuntimeID, beforeMult, afterMult)

	BroadcastStatusEffectDelta(area, target.RuntimeID, effect, tmpl, true, now)
	return true
}

// statusEffectTickWork is a snapshot of ONE effect's tick config, captured
// under act.Mu (tickStatusEffects, area.go) and applied AFTER that lock is
// released — ApplyDamage/ApplyHeal (spell.go) take act.Mu themselves.
type statusEffectTickWork struct {
	sourceRID uint32
	dmgMin    int32
	dmgMax    int32
	isHeal    bool
}

// applyStatusEffectTick resolves ONE DoT/HoT tick against target — rolls
// dmgMin..dmgMax and applies it through the EXACT same pipeline any other
// hit/heal uses (ApplyDamage/ApplyHeal, spell.go), so a DoT/HoT tick is
// indistinguishable from a normal hit/heal to the rest of the game.
//
// Order (Fase 2's lesson applied here too): ApplyDamage/ApplyHeal mutate
// target.Health and fully return BEFORE BroadcastHPUpdate is called — never
// the other way around, or the client would see a stale HP snapshot the
// same way it saw a stale MovementSpeedMult before that fix.
//
// A lethal DoT tick runs the SAME death pipeline a normal hit's justDied
// branch uses (see combat_special.go): BroadcastAnimate("Death") +
// BroadcastActorDead + OnNPCKilled (self-guards on !IsNPC) +
// runSpecialKillHook (mastery XP/quest progress/drops/respawn queue for
// NPCs, via net/combat_special_bridge.go's handleSpecialKill — already
// wired, nothing new needed there). Known gap: if sourceRID no longer
// resolves to a live *Actor (e.g. the caster disconnected/left the area
// while the DoT was still ticking), runSpecialKillHook is skipped — same
// no-op handleSpecialKill would do anyway on a nil attacker — so an NPC
// killed by an "orphaned" DoT won't get KillNPC'd/dropped. Rare edge case,
// not solved this round.
func applyStatusEffectTick(area *Area, target *Actor, sourceRID uint32, dmgMin, dmgMax int32, isHeal bool) {
	if area == nil || target == nil || target.IsDead() {
		return
	}

	amount := dmgMin
	if dmgMax > dmgMin {
		amount = dmgMin + rand.Int31n(dmgMax-dmgMin+1)
	}
	if amount <= 0 {
		return
	}

	if isHeal {
		hp := ApplyHeal(target, amount)
		BroadcastFloatingNumber(area, target, int16(amount), 1)
		BroadcastHPUpdate(area, target, hp)
		return
	}

	hp, justDied := ApplyDamage(target, amount, sourceRID)
	BroadcastFloatingNumber(area, target, int16(amount), 0)
	BroadcastHPUpdate(area, target, hp)

	if justDied {
		BroadcastAnimate(area, target, "Death")
		BroadcastActorDead(area, target.RuntimeID, sourceRID)
		OnNPCKilled(area, target, sourceRID)
		if sourceActor, ok := area.GetActor(sourceRID); ok {
			runSpecialKillHook(area, sourceActor, target)
		}
	}
}
