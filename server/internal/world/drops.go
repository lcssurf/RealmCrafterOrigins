package world

import (
	"math/rand"
	"sync/atomic"
	"time"
)

var dropRIDCounter uint32

func nextDropRID() uint32 {
	return atomic.AddUint32(&dropRIDCounter, 1) | 0x80000000
}

// DropEntry describes one possible item drop from an NPC.
type DropEntry struct {
	ItemID       uint16
	Name         string
	ItemType     uint8
	SlotType     uint8
	WeaponDamage int16
	ArmorLevel   int16
	ItemValue    int32
	Chance       float32
	MinQty, MaxQty uint8
}

// DroppedItem is a world-space loot item waiting to be picked up.
type DroppedItem struct {
	RuntimeID    uint32
	ItemID       uint16
	Quantity     uint8
	Name         string
	ItemType     uint8
	SlotType     uint8
	WeaponDamage int16
	ArmorLevel   int16
	ItemValue    int32
	X, Y, Z      float32
	SpawnedAt    int64 // unix ms
}

var npcDropTables = map[string][]DropEntry{}

// RegisterDropTable registers a drop table for the given NPC name.
func RegisterDropTable(npcName string, entries []DropEntry) {
	npcDropTables[npcName] = entries
}

// RollDrops rolls the NPC's drop table and returns items to spawn in the world.
func RollDrops(npcName string, x, y, z float32) []*DroppedItem {
	entries := npcDropTables[npcName]
	if len(entries) == 0 {
		return nil
	}
	var drops []*DroppedItem
	for _, e := range entries {
		if rand.Float32() < e.Chance {
			qty := e.MinQty
			if e.MaxQty > e.MinQty {
				qty = e.MinQty + uint8(rand.Intn(int(e.MaxQty-e.MinQty)+1))
			}
			drops = append(drops, &DroppedItem{
				RuntimeID:    nextDropRID(),
				ItemID:       e.ItemID,
				Quantity:     qty,
				Name:         e.Name,
				ItemType:     e.ItemType,
				SlotType:     e.SlotType,
				WeaponDamage: e.WeaponDamage,
				ArmorLevel:   e.ArmorLevel,
				ItemValue:    e.ItemValue,
				X:            x,
				Y:            y,
				Z:            z,
				SpawnedAt:    time.Now().UnixMilli(),
			})
		}
	}
	return drops
}
