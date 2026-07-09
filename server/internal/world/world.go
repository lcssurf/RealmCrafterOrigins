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

	// heightmapBasePath is set by LoadHeightmaps() and reused by
	// GetOrCreateArea() so areas created afterwards (e.g. when the first
	// NPC/waypoint/spawn-point for that area is loaded) also get their
	// heightmap attached immediately, instead of only areas that already
	// existed at the time LoadHeightmaps() ran once. Without this, NPCs
	// spawned into a not-yet-created area had Area.Heightmap == nil at
	// spawn time and kept their raw (un-snapped) spawn Y forever, until
	// AI movement re-sampled it — see SpawnNPC() below.
	heightmapBasePath string
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
// Newly created areas get their heightmap loaded immediately (if
// LoadHeightmaps() has already run and recorded a base path), so any actor
// spawned into this area right after creation can snap to terrain Y.
func (w *World) GetOrCreateArea(name string) *Area {
	w.mu.Lock()
	defer w.mu.Unlock()
	if area, ok := w.areas[name]; ok {
		return area
	}
	area := NewArea(name)
	if w.heightmapBasePath != "" {
		if hm, err := loadHeightmapForArea(w.heightmapBasePath, name); err == nil {
			area.Heightmap = hm
			log.Printf("world: loaded heightmap for %q (lazy, on area creation)", name)
		}
	}
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
	// Snap the authored spawn Y to terrain height, same as AI movement does
	// every tick (see moveNPCToPoint / AIWander / AIPatrol in area.go, which
	// all call area.Heightmap.SampleWorld). Without this, NPCs spawn at the
	// flat Y stored in npc_spawns/spawn_points and only "land" once they
	// first move or fight — this fixes both cases so terrain-irregular spawns
	// (e.g. mountains) start grounded. The x/z themselves are left untouched;
	// only y is corrected. Ground-only for now — there is no
	// flying/aquatic movement-type flag on NPCs anywhere in the codebase, so
	// every current NPC is expected to stand on terrain.
	if area.Heightmap != nil {
		y = area.Heightmap.SampleWorld(x, z)
	}

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

// loadHeightmapForArea resolves and loads heightmap.bin for one area name.
// Tries the area name with spaces replaced by underscores first (the usual
// folder convention), then the exact area name as a fallback.
func loadHeightmapForArea(basePath, areaName string) (*Heightmap, error) {
	folder := strings.ReplaceAll(areaName, " ", "_")
	path := filepath.Join(basePath, folder, "heightmap.bin")
	hm, err := LoadHeightmap(path)
	if err != nil {
		path2 := filepath.Join(basePath, areaName, "heightmap.bin")
		hm, err = LoadHeightmap(path2)
	}
	return hm, err
}

// LoadHeightmaps tries to load heightmap.bin for every known area, and
// records basePath so GetOrCreateArea() can load heightmaps for areas
// created afterwards (NPC spawns / waypoints / spawn points can each
// GetOrCreateArea a brand-new area after this runs). Call this once, early,
// right after world.New() and before any spawning — see main.go.
// Missing files are silently skipped (NPCs in that area keep fixed Y).
func (w *World) LoadHeightmaps(basePath string) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.heightmapBasePath = basePath
	for name, area := range w.areas {
		hm, err := loadHeightmapForArea(basePath, name)
		if err != nil {
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
