package world

import (
	"math/rand"
	"strings"
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
	dropModelPathMu sync.RWMutex
	dropModelPath   string
	dropModelScaleMu sync.RWMutex
	dropModelScale float32 = 1
	bloodFXMu sync.RWMutex
	bloodFX string
	bloodModeMu sync.RWMutex
	bloodMode string = "basic"
)

func normalizeBloodMode(mode string) string {
	mode = strings.ToLower(strings.TrimSpace(mode))
	if mode == "all" {
		return "all"
	}
	return "basic"
}

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

// SetDropModelPath stores the fallback mesh path used for rendering world drops.
// Empty path means world drops are rendered without a mesh.
func SetDropModelPath(path string) {
	dropModelPathMu.Lock()
	dropModelPath = path
	dropModelPathMu.Unlock()
}

// GetDropModelPath returns the configured fallback mesh path for world drops.
// Empty path means disabled/legacy behavior (no drop mesh).
func GetDropModelPath() string {
	dropModelPathMu.RLock()
	defer dropModelPathMu.RUnlock()
	return dropModelPath
}

// SetDropModelScale stores the configured uniform scale used for default world drops.
// A value <= 0 defaults to 1.0 (legacy behavior).
func SetDropModelScale(scale float32) {
	if scale <= 0 {
		scale = 1
	}
	dropModelScaleMu.Lock()
	dropModelScale = scale
	dropModelScaleMu.Unlock()
}

// GetDropModelScale returns the configured fallback scale for world drops.
// Default value is 1.0.
func GetDropModelScale() float32 {
	dropModelScaleMu.RLock()
	defer dropModelScaleMu.RUnlock()
	return dropModelScale
}

// SetBloodFX stores the configured blood VFX key used by hit FX.
// Empty key disables the hit FX.
func SetBloodFX(key string) {
	bloodFXMu.Lock()
	bloodFX = strings.TrimSpace(key)
	bloodFXMu.Unlock()
}

// GetBloodFX returns the configured blood VFX key for hit FX.
// Empty string means hit blood FX is disabled.
func GetBloodFX() string {
	bloodFXMu.RLock()
	defer bloodFXMu.RUnlock()
	return bloodFX
}

// SetBloodMode stores the blood FX mode. Allowed values: "basic", "all".
// Unknown values are normalized to "basic".
func SetBloodMode(mode string) {
	bloodModeMu.Lock()
	bloodMode = normalizeBloodMode(mode)
	bloodModeMu.Unlock()
}

// GetBloodMode returns the runtime blood FX mode.
// Returns "basic" when unset or invalid.
func GetBloodMode() string {
	bloodModeMu.RLock()
	defer bloodModeMu.RUnlock()
	return normalizeBloodMode(bloodMode)
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
