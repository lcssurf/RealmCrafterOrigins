package world

import (
	"context"
	"fmt"
	"log"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
)

// World manages all areas and issues runtime IDs.
type World struct {
	areas  map[string]*Area
	mu     sync.RWMutex
	nextID uint32
}

// New creates a World with a default "Starter Zone" area.
func New() *World {
	w := &World{
		areas: make(map[string]*Area),
	}
	w.areas["Starter Zone"] = NewArea("Starter Zone")
	return w
}

// StartAI launches the AI goroutine for every area in the world.
// Call once after spawning all NPCs.
func (w *World) StartAI(ctx context.Context) {
	w.mu.RLock()
	defer w.mu.RUnlock()
	for _, area := range w.areas {
		area.StartAI(ctx)
	}
}

// StartRegen launches the regen goroutine for every area in the world.
func (w *World) StartRegen(ctx context.Context) {
	w.mu.RLock()
	defer w.mu.RUnlock()
	for _, area := range w.areas {
		area.StartRegen(ctx)
	}
}

// GetOrCreateArea returns an existing area by name, creating it if absent.
func (w *World) GetOrCreateArea(name string) *Area {
	w.mu.Lock()
	defer w.mu.Unlock()
	if area, ok := w.areas[name]; ok {
		return area
	}
	area := NewArea(name)
	w.areas[name] = area
	return area
}

// GetArea returns an area by name.
func (w *World) GetArea(name string) (*Area, bool) {
	w.mu.RLock()
	defer w.mu.RUnlock()
	area, ok := w.areas[name]
	return area, ok
}

// NextRuntimeID allocates a unique session runtime ID (never returns 0).
func (w *World) NextRuntimeID() uint32 {
	for {
		id := atomic.AddUint32(&w.nextID, 1)
		if id != 0 {
			return id
		}
	}
}

// SpawnNPC creates a static NPC actor and registers it in the given area.
func (w *World) SpawnNPC(area *Area, name, race, class string, level int, x, y, z, yaw float32) *Actor {
	npc := NewActor()
	npc.RuntimeID     = w.NextRuntimeID()
	npc.Name          = name
	npc.Race          = race
	npc.Class         = class
	npc.Level         = uint16(level)
	npc.X, npc.Y, npc.Z = x, y, z
	npc.Yaw           = yaw
	npc.AreaName      = area.Name
	npc.IsNPC         = true
	npc.Aggressiveness  = 1   // defensive: fights back when attacked
	npc.AggressiveRange = 10.0
	npc.AttackRange     = 2.0 // tight melee; override after spawn for ranged NPCs
	npc.Radius          = 0.4
	npc.WeaponDamage  = int32(level) * 2
	npc.BasicAttackDim = DimMelee
	base := int32(level) * 3
	npc.SetPrimaryStats(PrimaryStats{
		STR: base,
		DEX: base,
		INT: base,
		WIS: base,
		PER: base,
	})
	RecomputeDerivedStats(npc, nil)
	npc.Health = npc.HealthMax
	npc.Energy = npc.EnergyMax
	npc.SpawnX, npc.SpawnY, npc.SpawnZ, npc.SpawnYaw = x, y, z, yaw
	npc.SpawnAreaName = area.Name
	npc.RespawnDelay  = 30_000 // 30 s
	area.AddActor(npc)
	return npc
}

// AddPortal registers a portal in the named area (creates the area if needed).
func (w *World) AddPortal(areaName string, p Portal) {
	area := w.GetOrCreateArea(areaName)
	area.Mu.Lock()
	area.Portals = append(area.Portals, p)
	area.Mu.Unlock()
}

// CheckPortal returns the first portal the actor is standing inside, or nil.
func (w *World) CheckPortal(actor *Actor, area *Area) *Portal {
	area.Mu.RLock()
	defer area.Mu.RUnlock()
	for i := range area.Portals {
		p := &area.Portals[i]
		dx := actor.X - p.X
		dz := actor.Z - p.Z
		if dx*dx+dz*dz <= p.Radius*p.Radius {
			return p
		}
	}
	return nil
}

// LoadHeightmaps tries to load heightmap.bin for every known area.
// basePath is the directory that contains the per-area subdirs
// (e.g. "../client/data/areas").  Missing files are silently skipped.
func (w *World) LoadHeightmaps(basePath string) {
	w.mu.RLock()
	defer w.mu.RUnlock()
	for name, area := range w.areas {
		// Folder name = area name with spaces replaced by underscores.
		folder := strings.ReplaceAll(name, " ", "_")
		path := filepath.Join(basePath, folder, "heightmap.bin")
		hm, err := LoadHeightmap(path)
		if err != nil {
			// Try exact area name as folder (no substitution).
			path2 := filepath.Join(basePath, name, "heightmap.bin")
			hm, err = LoadHeightmap(path2)
		}
		if err != nil {
			// No heightmap for this area — NPCs keep fixed Y.
			continue
		}
		area.Heightmap = hm
		log.Printf("world: loaded heightmap for %q (%s)", name, fmt.Sprintf("%dx%d cs=%.1f", hm.w, hm.h, hm.cellSize))
	}
}

// FindActor searches every area for the actor with the given runtime ID.
func (w *World) FindActor(runtimeID uint32) (*Actor, *Area) {
	w.mu.RLock()
	defer w.mu.RUnlock()
	for _, area := range w.areas {
		if actor, ok := area.GetActor(runtimeID); ok {
			return actor, area
		}
	}
	return nil, nil
}
