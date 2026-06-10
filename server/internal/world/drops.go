package world

import (
	"math/rand"
	"sync"
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

// LootTableRuntime is the in-memory representation of a loot table loaded from
// database rows.
type LootTableRuntime struct {
	Entries []DropEntry
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

var (
	lootCatalogMu sync.RWMutex
	lootCatalog   = make(map[int]*LootTableRuntime)
)

// SetLootCatalog replaces the in-memory loot catalog.
func SetLootCatalog(tables map[int]*LootTableRuntime) {
	next := make(map[int]*LootTableRuntime, len(tables))
	for id, table := range tables {
		if id <= 0 || table == nil || len(table.Entries) == 0 {
			continue
		}
		entries := make([]DropEntry, len(table.Entries))
		copy(entries, table.Entries)
		next[id] = &LootTableRuntime{Entries: entries}
	}
	lootCatalogMu.Lock()
	lootCatalog = next
	lootCatalogMu.Unlock()
}

// GetLootTable resolves one loot table from the in-memory catalog.
func GetLootTable(id int) (*LootTableRuntime, bool) {
	if id <= 0 {
		return nil, false
	}
	lootCatalogMu.RLock()
	table, ok := lootCatalog[id]
	lootCatalogMu.RUnlock()
	if !ok || table == nil || len(table.Entries) == 0 {
		return nil, false
	}

	out := &LootTableRuntime{}
	out.Entries = make([]DropEntry, len(table.Entries))
	copy(out.Entries, table.Entries)
	return out, true
}

// RollDropsByTable rolls the provided loot table and returns items to spawn in the world.
func RollDropsByTable(lootTableID int, x, y, z float32) []*DroppedItem {
	table, ok := GetLootTable(lootTableID)
	if !ok || table == nil || len(table.Entries) == 0 {
		return nil
	}
	var drops []*DroppedItem
	for _, e := range table.Entries {
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
