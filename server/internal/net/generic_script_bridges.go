package net

import "realm-crafter/server/internal/world"

// handleNPCSpawn implements world.NPCSpawnHook — registered as
// world.SetNPCSpawnHook in server.go. Generic scripting Fase 2 (item 4):
// fires on every NPC respawn (the initial startup spawn calls
// Registry.DispatchNPCSpawn directly from main.go instead, which already
// has both area and the registry in scope there).
func (s *Server) handleNPCSpawn(area *world.Area, npc *world.Actor) {
	if s == nil || s.scripting == nil || area == nil || npc == nil {
		return
	}
	s.scripting.DispatchNPCSpawn(npc, area)
}

// handleAbilityCast implements world.AbilityCastHook — registered as
// world.SetAbilityCastHook in server.go. Generic scripting Fase 2 (item 3):
// fires on every successful cast start, player or NPC.
func (s *Server) handleAbilityCast(area *world.Area, caster, target *world.Actor, abilityID int) {
	if s == nil || s.scripting == nil || area == nil || caster == nil {
		return
	}
	s.scripting.DispatchAbilityCast(caster, target, abilityID, area)
}
