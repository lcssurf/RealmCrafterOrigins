package net

import "realm-crafter/server/internal/world"

// handleZoneTrigger implements world.ZoneTriggerHook — registered as
// world.SetZoneTriggerHook in server.go. Generic scripting Fase 1: routes
// Area.tickAreaTriggers' enter/exit transitions to the matching Lua event
// (trigger_enter/trigger_exit — see Registry.DispatchTriggerEnter/Exit).
func (s *Server) handleZoneTrigger(area *world.Area, actor *world.Actor, triggerID int, entered bool) {
	if s == nil || s.scripting == nil || area == nil || actor == nil {
		return
	}
	if entered {
		s.scripting.DispatchTriggerEnter(actor, triggerID, area)
	} else {
		s.scripting.DispatchTriggerExit(actor, triggerID, area)
	}
}
