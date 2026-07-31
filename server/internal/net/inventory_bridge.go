package net

import (
	"context"
	"fmt"

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

// AddItem implements scripting.InventoryBridge — backs Lua's
// Inventory.add_item. Reuses db.AddStackableItem, the same call already used
// by pickup/shop-buy/quest-reward (net/client.go handlePickup/handleShopAction,
// net/quest_runtime.go) — maxStack=0 (look up item_templates), dur=100 (full
// durability), matching those call sites' defaults exactly.
func (s *Server) AddItem(playerRID uint32, itemID uint16, qty int) (bool, error) {
	c := s.findClientByRuntimeID(playerRID)
	if c == nil || c.actor == nil || c.actor.CharacterID == "" {
		return false, nil
	}
	if qty <= 0 || qty > 255 {
		return false, fmt.Errorf("scripting: AddItem qty=%d out of range (1-255)", qty)
	}
	if _, err := s.db.AddStackableItem(context.Background(), c.actor.CharacterID, itemID, uint8(qty), 0, 100); err != nil {
		return false, err
	}
	// Refresh the player's inventory view — same reason as RemoveItem above.
	_ = c.sendInventory(context.Background(), c.actor.CharacterID)
	return true, nil
}
