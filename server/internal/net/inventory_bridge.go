package net

import (
	"context"

	"realm-crafter/server/internal/scripting"
)

var _ scripting.InventoryBridge = (*Server)(nil)

// HasItem implements scripting.InventoryBridge — backs Lua's Inventory.has_item.
func (s *Server) HasItem(playerRID uint32, itemID uint16, qty int) (bool, error) {
	c := s.findClientByRuntimeID(playerRID)
	if c == nil || c.actor == nil || c.actor.CharacterID == "" {
		return false, nil
	}
	return s.db.HasItem(context.Background(), c.actor.CharacterID, itemID, qty)
}

// RemoveItem implements scripting.InventoryBridge — backs Lua's Inventory.remove_item.
func (s *Server) RemoveItem(playerRID uint32, itemID uint16, qty int) (bool, error) {
	c := s.findClientByRuntimeID(playerRID)
	if c == nil || c.actor == nil || c.actor.CharacterID == "" {
		return false, nil
	}
	ok, err := s.db.RemoveItemQty(context.Background(), c.actor.CharacterID, itemID, qty)
	if err != nil || !ok {
		return ok, err
	}
	// Refresh the player's inventory view — mirrors handleUseItem/
	// handleInventorySwap, which always end with sendInventory after a
	// mutation so the client's bag UI stays in sync.
	_ = c.sendInventory(context.Background(), c.actor.CharacterID)
	return true, nil
}
