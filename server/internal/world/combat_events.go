// Package world - combat_events.go
//
// Combat event and animation broadcasts.
package world

import (
	"log"
	"strings"
)

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
	var actionID uint8 = 0xFF
	for i, anim := range actor.Appearance.Anims {
		if anim.Action == actionName {
			actionID = uint8(i)
			break
		}
	}
	if actionID == 0xFF {
		log.Printf("animate: warning actor=%d action=%q action_id=%d missing_action_binding=true",
			actor.RuntimeID, actionName, -1)
		return
	}
	log.Printf("animate: actor=%d action=%q action_id=%d", actor.RuntimeID, actionName, actionID)
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
