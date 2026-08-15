package world

import "sync"

// ZoneTriggerHook is implemented by the net package and invoked whenever a
// player enters or exits a Trigger volume (Area.tickAreaTriggers) —
// generic scripting Fase 1. Mirrors SetSpecialKillHook/SetStatusEffectHook's
// "world package owns WHEN, net package owns HOW to reach Lua" split.
type ZoneTriggerHook func(area *Area, actor *Actor, triggerID int, entered bool)

var (
	zoneTriggerHookMu sync.RWMutex
	zoneTriggerHook   ZoneTriggerHook
)

// SetZoneTriggerHook registers the callback invoked on every trigger
// enter/exit transition.
func SetZoneTriggerHook(h ZoneTriggerHook) {
	zoneTriggerHookMu.Lock()
	zoneTriggerHook = h
	zoneTriggerHookMu.Unlock()
}

func runZoneTriggerHook(area *Area, actor *Actor, triggerID int, entered bool) {
	zoneTriggerHookMu.RLock()
	h := zoneTriggerHook
	zoneTriggerHookMu.RUnlock()
	if h == nil {
		return
	}
	h(area, actor, triggerID, entered)
}
