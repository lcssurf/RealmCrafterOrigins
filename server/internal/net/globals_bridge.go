package net

import (
	"context"

	"realm-crafter/server/internal/scripting"
)

var _ scripting.GlobalsBridge = (*Server)(nil)

// SetActorGlobal implements scripting.GlobalsBridge — backs Lua's
// Actor.set_global. No-op (nil error) if the RID doesn't resolve to a
// connected, logged-in character — same "quietly do nothing" convention
// InventoryBridge already uses (see inventory_bridge.go).
func (s *Server) SetActorGlobal(playerRID uint32, key, value string) error {
	c := s.findClientByRuntimeID(playerRID)
	if c == nil || c.actor == nil || c.actor.CharacterID == "" {
		return nil
	}
	return s.db.SetActorGlobal(context.Background(), c.actor.CharacterID, key, value)
}

// GetActorGlobal implements scripting.GlobalsBridge — backs Lua's
// Actor.get_global.
func (s *Server) GetActorGlobal(playerRID uint32, key string) (string, error) {
	c := s.findClientByRuntimeID(playerRID)
	if c == nil || c.actor == nil || c.actor.CharacterID == "" {
		return "", nil
	}
	return s.db.GetActorGlobal(context.Background(), c.actor.CharacterID, key)
}

// SetWorldGlobal implements scripting.GlobalsBridge — backs Lua's
// World.set_global. Per-area, not per-connection — no actor lookup needed.
func (s *Server) SetWorldGlobal(areaName, key, value string) error {
	return s.db.SetWorldGlobal(context.Background(), areaName, key, value)
}

// GetWorldGlobal implements scripting.GlobalsBridge — backs Lua's
// World.get_global.
func (s *Server) GetWorldGlobal(areaName, key string) (string, error) {
	return s.db.GetWorldGlobal(context.Background(), areaName, key)
}
