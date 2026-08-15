// Package world - combat_events.go
//
// Combat event and animation broadcasts.
package world

import (
	"log"
	"strings"
)

const logAnimateBroadcast = false

// maxFallbackDepth caps the animation vocabulary fallback walk. The seed
// tree's max depth is 3 (e.g. Attack->Shoot->BowDraw); 8 gives headroom while
// bounding a misconfigured cycle (A->B->A) instead of looping forever.
const maxFallbackDepth = 8

// Combat event codes mirrored from protocol (avoids circular import).
const (
	combatEventGuardEnded     uint8 = 4
	combatEventHitDodged      uint8 = 8
	combatEventHitGuarded     uint8 = 9
	combatEventHitParried     uint8 = 10
	combatEventSpecialWindup  uint8 = 11
	combatEventSpecialParry   uint8 = 12
	combatEventSpecialHit     uint8 = 13
	combatEventCritHit        uint8 = 14
	combatEventSpecialCritHit uint8 = 15
)

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
	// Fallback cascade: try the requested action, then walk up the vocabulary tree
	// (AnimFallbackParent) until a binding is found or we reach a root. The depth
	// cap guards against a misconfigured cycle (A->B->A) in the vocabulary.
	var actionID uint8 = 0xFF
	resolved := actionName
	for i := 0; i < maxFallbackDepth; i++ {
		for idx, anim := range actor.Appearance.Anims {
			if anim.Action == resolved {
				actionID = uint8(idx)
				break
			}
		}
		if actionID != 0xFF || resolved == "" {
			break
		}
		resolved = AnimFallbackParent(resolved)
	}
	// Textual last-resort fallback: the explicit tree (parent_id chain) is
	// already exhausted at this point — resolved=="" means either a genuine
	// registered root had no binding, OR (the common gap, see
	// docs/TECH_DEBT.md #100-103 and this session's Attack_sword_1h/
	// Idle_sword_1h incident) the requested name was never registered as a
	// node in anim_vocabulary at all, so AnimFallbackParent couldn't even
	// start walking. Rather than let gameplay break on a missing GUE step
	// ("Add Weapon Bindings" never run for a given weapon_anim_style), strip
	// everything from the first "_" onward and try that prefix directly
	// against the actor's own bindings — "Attack_sword_1h" -> "Attack",
	// which the seed vocabulary guarantees exists. This does NOT consult
	// AnimFallbackParent again (the prefix itself may not be a tree node
	// either — that's irrelevant, we just need a literal binding match) and
	// does NOT run if the explicit tree already found something.
	usedHeuristicFallback := false
	var heuristicPrefix string
	if actionID == 0xFF {
		if idx := strings.IndexByte(actionName, '_'); idx > 0 {
			heuristicPrefix = actionName[:idx]
			for idx2, anim := range actor.Appearance.Anims {
				if anim.Action == heuristicPrefix {
					actionID = uint8(idx2)
					resolved = heuristicPrefix
					usedHeuristicFallback = true
					break
				}
			}
		}
	}

	if actionID == 0xFF {
		log.Printf("animate: warning actor=%d action=%q missing_action_binding=true (fallback exhausted, heuristic prefix %q also missing)",
			actor.RuntimeID, actionName, heuristicPrefix)
		return
	}
	if usedHeuristicFallback {
		log.Printf("animate: actor=%d action=%q used heuristic prefix fallback to %q action_id=%d — "+
			"the explicit vocabulary tree has no node for %q (run \"Add Weapon Bindings\" in the GUE's "+
			"Animation Vocabulary tab to configure this properly instead of relying on this heuristic)",
			actor.RuntimeID, actionName, resolved, actionID, actionName)
	} else if resolved != actionName {
		log.Printf("animate: actor=%d action=%q resolved via fallback to %q action_id=%d",
			actor.RuntimeID, actionName, resolved, actionID)
	}

	// Suppress repeated locomotion broadcasts for the same state.
	// One-shot actions (Attack/Cast/etc.) are still sent every time.
	// Prefix match (not ==) so composite names ("Idle_Sword1H", weapon-style
	// aware — see Actor.IdleAction) still count as locomotion and dedup
	// correctly; an exact-match check here would treat every composite Idle
	// as a fresh one-shot and never suppress repeats.
	isLocomotion := strings.HasPrefix(actionName, "Idle") ||
		strings.HasPrefix(actionName, "Walk") ||
		strings.HasPrefix(actionName, "Run")
	actor.Mu.Lock()
	if isLocomotion && actor.CurrentAction == actionName {
		actor.Mu.Unlock()
		return
	}
	actor.CurrentAction = actionName
	actor.Mu.Unlock()

	if logAnimateBroadcast {
		log.Printf("animate: actor=%d action=%q action_id=%d", actor.RuntimeID, actionName, actionID)
	}

	// TEMP DEBUG (sword_1h Attack-not-falling-back-visually investigation) —
	// unconditional (not gated by logAnimateBroadcast) final confirmation of
	// exactly what's being sent over the wire for THIS broadcast: the
	// requested name, what it resolved to (explicit tree / heuristic /
	// direct hit), and the action_id (index into actor.Appearance.Anims,
	// the SAME array serialized into PNewActor — see frame.go
	// appendAnimBindings) that the client will receive. Remove once the
	// regression is confirmed/fixed.
	log.Printf("[wire-confirm] actor=%d requested=%q final_resolved=%q action_id=%d bound_action=%q",
		actor.RuntimeID, actionName, resolved, actionID, actor.Appearance.Anims[actionID].Action)

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
