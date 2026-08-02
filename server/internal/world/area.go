package world

import (
	"context"
	"log"
	"math"
	"math/rand"
	"sync"
	"sync/atomic"
	"time"
)

type deadNPC struct {
	actor  *Actor
	DeadAt int64 // unix ms
}

// Waypoint is one node in an NPC patrol graph. NextA/NextB are IDs of
// successor waypoints (0 = end); if both are set one is chosen at random.
type Waypoint struct {
	ID      int
	X, Y, Z float32
	NextA   int // ID of next waypoint (0 = end of path)
	NextB   int // ID of alternate branch (0 = no branch)
	PauseMs int // ms to pause at this node
}

// WorldObject is a placed static model instance in a zone.
type WorldObject struct {
	// ID is the zone_scenery row id (see db.WorldObject.ID / LoadWorldObjects).
	// Used to key Area.objectOverrides and as the object_id argument to the
	// Lua World.SetTransform/SetVisible/SetCollision API.
	ID        int
	ModelPath string
	Scale     float32
	X, Y, Z   float32
	Yaw       float32
	// Pitch/Roll: the zone_scenery.pitch/roll columns (X/Z axis rotation —
	// see zone_scenery.h ZScenery::rot "pitch/yaw/roll degrees" and
	// ZoneRenderer's Ry*Rx*Rz scenery matrix build). Previously never loaded
	// here — LoadWorldObjects only selected yaw, so any object placed
	// pitched/rolled in the GUE (e.g. upside-down) rendered upright in the
	// game client despite showing correctly in the GUE preview.
	Pitch, Roll float32
	// BlackCutout: OR of media_models.black_cutout | the scenery's override
	// material's black_cutout (mirrors the same flag on actor mesh slots —
	// see appearance.go). When true the client discards near-black pixels
	// in the deferred gBuffer pass (foliage/leaf cutout).
	BlackCutout bool
	// Interactable: zone_scenery.interactable — gates whether the client's
	// reticle hit-test offers "[F] Interagir"/sends PObjectInteract for
	// this object at all. Opt-in (default false in the DB): most scenery
	// is decorative and has no object_interact Lua handler bound to it.
	Interactable bool
}

// WorldObjectState holds the current transform/visibility/collision of a
// WorldObject that has diverged from its static authored default. Written by
// the Lua World.SetTransform/SetVisible/SetCollision API (see
// scripting/world_api.go) and merged into the PWorldObjects snapshot sent to
// clients entering the area, so an object always loads in its current (not
// original) state. Mirrors the Area.droppedItems pattern.
type WorldObjectState struct {
	X, Y, Z     float32
	Yaw         float32
	Pitch, Roll float32 // never touched by SetTransform (yaw-only) — carried over from the static/base object so it isn't lost on override
	Scale       float32
	Visible     bool
	Collision   bool
}

// Light is a placed static point light (torch/lantern) in a zone. Sent to
// clients on area load; the client resubmits every light every frame into
// its deferred Pipeline::AddPointLight() light-volume pass (that pipeline
// code already existed and was fully functional but unused before this).
// Point-light Phase 1 — see doc/TECH_DEBT.md for Phase 2 (dynamic skill/FX
// lights, reusing this same wire format) and point-light shadows (not
// implemented at all yet, directional-only).
type Light struct {
	Name      string
	X, Y, Z   float32
	ColorR, ColorG, ColorB float32
	Intensity float32
	Radius    float32
	// LightType/Yaw/Pitch/ConeAngle — additive (Phase 2, Spot/Directional).
	// LightType 0=Point (default, everything below unused — byte-identical
	// to Phase 1 behavior) 1=Spot 2=Directional. Yaw/Pitch: degrees, same
	// Ry*Rx convention as scenery/NPC orientation (no roll — a light's
	// aim direction doesn't need it). ConeAngle: Spot only, full cone
	// angle in degrees.
	LightType int
	Yaw       float32
	Pitch     float32
	ConeAngle float32
}

// Emitter is a placed static particle emitter (zone_emitters bound to a real
// fx_templates entry via FXTemplateID) — sent once on area entry, resolved
// client-side to an FXParams from its already-loaded fx_catalog (kPFXCatalog)
// and spawned via ParticleSystem::SpawnEmitterParams with duration=-1 when
// Loop is true (never expires — see shared/renderer particles.h/.cpp).
type Emitter struct {
	FXTemplateID int
	X, Y, Z      float32
	Loop         bool
}

// Water is a placed static water plane — Gerstner wave vertex displacement
// (water.vs) + depth-based shallow/deep color transparency + procedural
// shoreline foam (Sub-fase 2a/2b, water.fs sampling gDepth_ via
// Pipeline::SceneDepthTexture()). Sent to clients on area load; the
// client's WaterManager draws one grid mesh per instance using
// water.vs/water.fs.
// See doc/TECH_DEBT.md #117 for later phases (reflection/refraction) and
// the swim-detection mechanic.
type Water struct {
	X, Y, Z                float32
	ScaleX, ScaleZ         float32
	ColorR, ColorG, ColorB float32
	Opacity                float32 // 0-1
	TexPath                string
	TexScale               float32
	// Gerstner wave tunables (no normal map file).
	WaveSpeed          float32
	WaveDirX, WaveDirZ float32
	WaveScale          float32
	// Sub-fase 2a — depth-based transparency.
	ShallowR, ShallowG, ShallowB float32
	DeepR, DeepG, DeepB          float32
	DepthFadeDistance            float32
	// Sub-fase 2b — procedural shoreline foam. Reuses 2a's depth
	// comparison (no separate depth field/logic here).
	FoamWidth           float32
	FoamR, FoamG, FoamB float32
}

// AtmosphereVolume is a placed region ("Post Process Volume" style) that
// overrides the area's atmosphere while the player is inside it. Fase 1
// (this struct + editor + wire format) is data-only — sent to the client on
// area load same as Lights/Water, but the client only stores it for now; the
// point-in-volume test and the enter/exit blend (using TransitionSeconds)
// are Fase 2. Field set mirrors Area's own Fog*/Ambient* plus the fuller
// render-tuning section AreaConfig sends via PAreaConfig (sun/scene/color/
// char) — see server/internal/db/db.go AtmosphereVolume for the DB side and
// tools/gue/src/zone_scene.h ZAtmosphereVolume for the authoring side.
type AtmosphereVolume struct {
	Name  string
	Shape int // 0=AABB (pos±size/2) 1=sphere (radius=SizeX/2)

	PosX, PosY, PosZ    float32
	SizeX, SizeY, SizeZ float32

	Priority          int
	TransitionSeconds float32

	SunDirX, SunDirY, SunDirZ       float32
	SunColorR, SunColorG, SunColorB float32
	SunIntensityMul                 float32
	SkyIntensityMul                 float32
	FogDensityMul                   float32
	FogR, FogG, FogB                float32
	AmbientR, AmbientG, AmbientB    uint8
	Volumetrics                     bool

	CharShadowLift   float32
	CharRimStrength  float32
	CharRimExponent  float32
	CharMinNdotL     float32
	CharAmbientBoost float32

	SceneIblIntensity       float32
	SceneSkyIntensity       float32
	SceneWorldShadowLift    float32
	SceneDirectScale        float32
	SceneAmbientScale       float32
	SceneFlatAmbient        float32
	SceneWorldMinNdotL      float32
	SceneAlbedoMinLuma      float32
	SceneAlbedoLiftStrength float32
	SceneSpecularScale      float32
	SceneExposureFactor     float32
	SceneSunIntensity       float32

	ColorContrast         float32
	ColorSaturation       float32
	ColorVibrance         float32
	ColorBlackPoint       float32
	ColorVignetteStrength float32
	ColorVignetteSoftness float32
}

// DynamicShape is one local-space box/wedge collision shape (from
// media_model_shapes, unrotated/unscaled model-local coordinates) belonging
// to a WorldObject flagged is_dynamic=1 in the GUE. Sent to the client once
// on area entry (PDynamicCollisionShapes) instead of being baked into
// coldata.bin — the client re-derives the world-space box every frame from
// the object's current (possibly script-animated) pose.
type DynamicShape struct {
	Type                       uint8 // 0=box 3=wedge (mesh/sphere never sent — see LoadDynamicCollisionShapes)
	OffsetX, OffsetY, OffsetZ  float32
	SizeX, SizeY, SizeZ        float32
	DetailA, DetailB           float32
}

// DynamicCollisionObject groups all DynamicShapes belonging to one WorldObject.
type DynamicCollisionObject struct {
	ObjectID int
	Shapes   []DynamicShape
}

// Trigger is a script-activated zone volume (XZ cylinder).
type Trigger struct {
	ID          int
	X, Z        float32
	Radius      float32
	Script      string
	Func        string
	TriggerOnce bool
	// fired tracks which actor runtime IDs have already fired this (for TriggerOnce).
	fired map[uint32]bool
}

// Area represents a game zone.
type Area struct {
	Name      string
	actors    map[uint32]*Actor
	Mu        sync.RWMutex
	Portals   []Portal
	Waypoints map[int]*Waypoint
	Objects   []WorldObject
	Lights    []Light
	Emitters  []Emitter
	Water     []Water
	AtmosphereVolumes []AtmosphereVolume
	DynamicCollisionObjects []DynamicCollisionObject

	// Environment config (loaded from area_config at startup)
	PvPEnabled                   bool
	IsOutdoor                    bool
	FogNear                      float32
	FogFar                       float32
	FogR, FogG, FogB             float32 // 0.0–1.0
	AmbientR, AmbientG, AmbientB uint8
	Gravity                      float32
	EntryScript                  string
	ExitScript                   string
	MusicTrack                   uint8

	// Script trigger volumes
	Triggers []Trigger

	// Heightmap for server-side Y sampling (nil for areas without terrain).
	Heightmap *Heightmap

	// Dead NPCs waiting to respawn.
	dnmu     sync.Mutex
	deadNPCs []deadNPC

	// Dropped items sitting in the world.
	diMu         sync.Mutex
	droppedItems map[uint32]*DroppedItem

	// Runtime overrides for WorldObjects modified at runtime via Lua
	// (World.SetTransform/SetVisible/SetCollision). Mirrors droppedItems.
	woMu            sync.Mutex
	objectOverrides map[int]*WorldObjectState

	// Scheduled one-shot callbacks (Lua Timer.after) — checked once per AI
	// tick (see tickAI/tickScheduledCalls). fn is a plain Go closure so this
	// package never needs to import the scripting/gopher-lua types; the
	// scripting package's Timer.after binding supplies a closure that itself
	// re-enters the Lua state safely (locks Registry.mu, sets callCtx, calls
	// the Lua function, resets callCtx) — mirrors droppedItems/deadNPCs.
	scMu      sync.Mutex
	scheduled []scheduledCall

	// Ephemeral (script-spawned) WorldObjects — Lua World.SpawnTempProp.
	// Separate map (own ID space, see NewEphemeralObjectID) from the
	// authored a.Objects/objectOverrides pair: these are NEVER zone_scenery
	// rows, never persisted, and always short-lived (auto-removed by the
	// ScheduleCall despawn timer set up in SpawnEphemeralObject). Mirrors
	// droppedItems/objectOverrides in shape only.
	eoMu             sync.Mutex
	ephemeralObjects map[int]*EphemeralObject
}

// EphemeralObject is one script-spawned, temporary WorldObject in flight —
// e.g. a meteor mesh falling from start to end over duration. Not
// zone_scenery-backed; exists only in memory/network for its lifetime. See
// Area.SpawnEphemeralObject and docs/TECH_DEBT.md, FX.play + SpawnTempProp
// investigation.
type EphemeralObject struct {
	ID          int
	ModelPath   string
	Scale       float32
	StartX, StartY, StartZ, StartYaw float32
	EndX, EndY, EndZ, EndYaw         float32
	SpawnedAt   int64 // unix ms
	DurationMs  int64
	OnArrivalFXKey string // "" = no FX fired on arrival
}

// kEphemeralObjectIDBase + a monotonic counter gives ephemeral objects an ID
// space that can never collide with a real zone_scenery row id (those are
// small positive DB autoincrement values — zone_scenery will never have a
// billion rows). Simpler and safer than negative IDs, which would need a
// signed/unsigned round-trip through WorldObjectsPayload's uint32 encoding.
const kEphemeralObjectIDBase = 1_000_000_000

var nextEphemeralObjectSeq int64

// NewEphemeralObjectID hands out a process-wide unique id for one
// EphemeralObject. sync/atomic, not a mutex — mirrors NewImpactID
// (actor.go) for the same reason: called from arbitrary goroutines with no
// natural shared lock.
func NewEphemeralObjectID() int {
	return kEphemeralObjectIDBase + int(atomic.AddInt64(&nextEphemeralObjectSeq, 1))
}

type scheduledCall struct {
	runAt int64 // unix ms
	fn    func()
}

// CheckTrigger returns the first trigger that contains the actor's XZ position,
// or nil if none. For TriggerOnce triggers, the actor's runtime ID is recorded
// and the trigger won't fire again for that actor.
func (a *Area) CheckTrigger(actor *Actor) *Trigger {
	a.Mu.RLock()
	defer a.Mu.RUnlock()
	for i := range a.Triggers {
		t := &a.Triggers[i]
		dx := actor.X - t.X
		dz := actor.Z - t.Z
		if dx*dx+dz*dz <= t.Radius*t.Radius {
			if t.TriggerOnce {
				if t.fired == nil {
					t.fired = make(map[uint32]bool)
				}
				if t.fired[actor.RuntimeID] {
					continue
				}
				t.fired[actor.RuntimeID] = true
			}
			return t
		}
	}
	return nil
}

// NewArea creates an empty Area.
func NewArea(name string) *Area {
	return &Area{
		Name:         name,
		actors:       make(map[uint32]*Actor),
		droppedItems: make(map[uint32]*DroppedItem),
		Waypoints:    make(map[int]*Waypoint),
		objectOverrides:  make(map[int]*WorldObjectState),
		ephemeralObjects: make(map[int]*EphemeralObject),
	}
}

// AddActor inserts an actor into the area.
func (a *Area) AddActor(actor *Actor) {
	a.Mu.Lock()
	defer a.Mu.Unlock()
	a.actors[actor.RuntimeID] = actor
}

// RemoveActor removes an actor by runtime ID.
func (a *Area) RemoveActor(runtimeID uint32) {
	a.Mu.Lock()
	defer a.Mu.Unlock()
	delete(a.actors, runtimeID)
}

// GetActor returns the actor with the given runtime ID, if present.
func (a *Area) GetActor(runtimeID uint32) (*Actor, bool) {
	a.Mu.RLock()
	defer a.Mu.RUnlock()
	actor, ok := a.actors[runtimeID]
	return actor, ok
}

// Broadcast sends data to every player (non-NPC) except exceptID (0 = send to all).
func (a *Area) Broadcast(data []byte, exceptID uint32) {
	a.Mu.RLock()
	defer a.Mu.RUnlock()
	for id, actor := range a.actors {
		if id == exceptID || actor.IsNPC {
			continue
		}
		actor.Send(data)
	}
}

// BroadcastAll sends data to every actor.
func (a *Area) BroadcastAll(data []byte) { a.Broadcast(data, 0) }

// Snapshot returns a copy of all actors currently in the area.
func (a *Area) Snapshot() []*Actor {
	a.Mu.RLock()
	defer a.Mu.RUnlock()
	out := make([]*Actor, 0, len(a.actors))
	for _, actor := range a.actors {
		out = append(out, actor)
	}
	return out
}

// StartRegen launches the HP/MP/SP regeneration goroutine for this area.
func (a *Area) StartRegen(ctx context.Context) {
	go func() {
		ticker := time.NewTicker(3 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				a.tickRegen()
			}
		}
	}()
}

const regenCombatWindow = int64(5_000) // ms out-of-combat before regen starts
const regenTickSeconds = 3.0           // must match StartRegen's ticker interval

func (a *Area) tickRegen() {
	now := time.Now().UnixMilli()
	masteryCfg := GetMasteryKillScalingConfig()
	GetCombatWindowManager().CleanupExpired(masteryCfg.WindowTimeoutMs)
	actors := a.Snapshot()
	for _, actor := range actors {
		actor.Mu.Lock()
		dead := actor.DeadAt > 0
		inCombat := actor.LastCombatAt > 0 && now-actor.LastCombatAt < regenCombatWindow
		hp, hpMax := actor.Health, actor.HealthMax
		mp, mpMax := actor.Energy, actor.EnergyMax
		sp, spMax := actor.Stamina, actor.StaminaMax
		healthRegen := actor.Derived.HealthRegen
		energyRegen := actor.Derived.EnergyRegen
		actor.Mu.Unlock()

		if dead {
			continue
		}

		var hpGain, mpGain, spGain int32
		if !inCombat {
			if hp < hpMax {
				hpGain = int32(healthRegen * regenTickSeconds)
				if hpGain < 1 {
					hpGain = 1
				}
			}
			if mp < mpMax {
				mpGain = int32(energyRegen * regenTickSeconds)
				if mpGain < 1 {
					mpGain = 1
				}
			}
		}
		if sp < spMax {
			spGain = spMax / 6 // faster stamina recovery
			if spGain < 1 {
				spGain = 1
			}
		}
		if hpGain == 0 && mpGain == 0 && spGain == 0 {
			continue
		}

		actor.Mu.Lock()
		if hpGain > 0 {
			actor.Health += hpGain
			if actor.Health > actor.HealthMax {
				actor.Health = actor.HealthMax
			}
			hp = actor.Health
		}
		if mpGain > 0 {
			actor.Energy += mpGain
			if actor.Energy > actor.EnergyMax {
				actor.Energy = actor.EnergyMax
			}
			mp = actor.Energy
		}
		if spGain > 0 {
			actor.Stamina += spGain
			if actor.Stamina > actor.StaminaMax {
				actor.Stamina = actor.StaminaMax
			}
			sp = actor.Stamina
		}
		actor.Mu.Unlock()

		BroadcastHPUpdate(a, actor, hp)
		if !actor.IsNPC && mpGain > 0 {
			BroadcastMPUpdate(actor, mp)
		}
		if !actor.IsNPC && spGain > 0 {
			BroadcastSPUpdate(actor, sp)
		}
	}
}

// StartAI launches the NPC AI goroutine for this area.
func (a *Area) StartAI(ctx context.Context) {
	go func() {
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()
		lastTick := time.Now()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				nowTick := time.Now()
				tickSec := float32(nowTick.Sub(lastTick).Seconds())
				lastTick = nowTick
				if tickSec <= 0 {
					tickSec = aiTickSecBase
				}
				if tickSec < aiTickSecMin {
					tickSec = aiTickSecMin
				}
				if tickSec > aiTickSecMax {
					tickSec = aiTickSecMax
				}
				a.tickAI(tickSec)
			}
		}
	}()
}

const (
	npcMoveSpeed  = 5.0  // world units per second
	aiTickSecBase = 0.1  // nominal tick seconds (100ms ticker)
	aiTickSecMin  = 0.05 // clamp low jitter
	aiTickSecMax  = 0.20 // clamp high jitter; avoids burst-jumps after stalls
	leashMultiple = 2.5  // NPC leashes when player is > aggro * leashMultiple from NPC spawn
)

// broadcastNPCPosition sends a PStandardUpdate for the NPC to every player in the area.
func broadcastNPCPosition(a *Area, npc *Actor) {
	var p pb
	p.u32(npc.RuntimeID)
	p.f32(npc.X)
	p.f32(npc.Y)
	p.f32(npc.Z)
	p.f32(npc.Yaw)
	p.u8(0) // flags
	a.BroadcastAll(buildFrame(pStandardUpdate, p))
}

// moveNPCToward advances npc one AI step toward target and updates its yaw.
// Returns true if the NPC moved (was not already in attack range).
func moveNPCToward(npc, target *Actor, a *Area, tickSec float32) {
	moveNPCToPoint(npc, target.X, target.Z, a, tickSec)
}

// moveNPCToPoint advances npc one AI step toward a world point and updates yaw.
func moveNPCToPoint(npc *Actor, targetX, targetZ float32, a *Area, tickSec float32) {
	dx := targetX - npc.X
	dz := targetZ - npc.Z
	dist := float32(math.Sqrt(float64(dx*dx + dz*dz)))
	if dist < 0.05 {
		return
	}
	if tickSec <= 0 {
		tickSec = aiTickSecBase
	}
	step := float32(npcMoveSpeed * tickSec)
	if step > dist {
		step = dist
	}
	npc.X += (dx / dist) * step
	npc.Z += (dz / dist) * step
	if a.Heightmap != nil {
		npc.Y = a.Heightmap.SampleWorld(npc.X, npc.Z)
	}
	// Face the movement direction.
	yaw := float32(math.Atan2(float64(dx), float64(dz)) * 180.0 / math.Pi)
	if yaw < 0 {
		yaw += 360
	}
	npc.Yaw = yaw
}

// leashNPC clears chase/combat transient state and starts a walking return to
// spawn instead of teleporting.
func leashNPC(npc *Actor, a *Area) {
	npc.Mu.Lock()
	npc.Guarding = false
	npc.GuardUntil = 0
	npc.ParryUntil = 0
	npc.DodgeUntil = 0
	npc.AITarget = nil
	npc.PendingImpacts = nil
	npc.SpecialChainCount = 0
	npc.AbilityCooldowns = make(map[int]int64)
	npc.AIMode = AIReturn
	npc.WaypointPauseUntil = 0
	npc.Mu.Unlock()
	BroadcastAnimate(a, npc, "Walk")
}

// pickWanderTarget selects a new random XZ destination within the NPC's wander
// radius of its spawn point. Must be called before entering AIWander.
func pickWanderTarget(npc *Actor) {
	angle := rand.Float64() * 2 * math.Pi
	r := rand.Float64() * float64(npc.WanderRadius)
	npc.WanderTargetX = npc.SpawnX + float32(r*math.Cos(angle))
	npc.WanderTargetZ = npc.SpawnZ + float32(r*math.Sin(angle))
}

// startWander picks a fresh wander destination and sets AIWander mode.
// Must be called with npc.Mu held.
func startWander(npc *Actor) {
	pickWanderTarget(npc)
	npc.AIMode = AIWander
}

// postChaseMode returns the AI mode to enter after a chase ends (or leash fires).
// Priority: patrol > wander > idle. Must be called with npc.Mu held.
func postChaseMode(npc *Actor) {
	npc.AITarget = nil
	npc.PendingImpacts = nil
	npc.SpecialChainCount = 0
	if npc.StartWaypointID > 0 {
		npc.CurrentWaypointID = npc.StartWaypointID
		npc.AIMode = AIPatrol
	} else if npc.WanderRadius > 0 {
		startWander(npc)
	} else {
		npc.AIMode = AIWait
	}
}

// endChase clears the NPC's chase target and transitions back to patrol, wander,
// or idle. Must be called with npc.Mu held.
func endChase(npc *Actor) { postChaseMode(npc) }

// tickAI runs one AI update step for all NPCs in the area.
func (a *Area) tickAI(tickSec float32) {
	now := time.Now().UnixMilli()
	a.tickDropDespawn(now)
	a.tickScheduledCalls(now)

	// Snapshot live NPCs.
	a.Mu.RLock()
	npcs := make([]*Actor, 0, len(a.actors))
	players := make([]*Actor, 0, len(a.actors))
	for _, act := range a.actors {
		if act.IsNPC {
			npcs = append(npcs, act)
		} else {
			players = append(players, act)
		}
	}
	a.Mu.RUnlock()

	// Check respawn queue.
	a.dnmu.Lock()
	remaining := a.deadNPCs[:0]
	for _, dn := range a.deadNPCs {
		if dn.actor.RespawnDelay > 0 && now-dn.DeadAt >= dn.actor.RespawnDelay {
			a.respawnNPC(dn.actor)
		} else {
			remaining = append(remaining, dn)
		}
	}
	a.deadNPCs = remaining
	a.dnmu.Unlock()

	var toKill []*Actor

	for _, npc := range npcs {
		npc.Mu.Lock()
		mode := npc.AIMode
		target := npc.AITarget
		npc.Mu.Unlock()

		switch mode {
		case AIWait:
			npc.Mu.Lock()
			if npc.StartWaypointID > 0 {
				npc.CurrentWaypointID = npc.StartWaypointID
				npc.AIMode = AIPatrol
			} else if npc.WanderRadius > 0 {
				startWander(npc)
			}
			npc.Mu.Unlock()
			if npc.Aggressiveness == 2 {
				a.lookForTarget(npc, players)
			}

		case AIWander:
			if npc.Aggressiveness == 2 {
				a.lookForTarget(npc, players)
				npc.Mu.Lock()
				mode = npc.AIMode
				npc.Mu.Unlock()
				if mode == AIChase {
					continue
				}
			}
			dx := npc.WanderTargetX - npc.X
			dz := npc.WanderTargetZ - npc.Z
			if dx*dx+dz*dz < 0.25 { // arrived
				npc.X = npc.WanderTargetX
				npc.Z = npc.WanderTargetZ
				broadcastNPCPosition(a, npc)
				BroadcastAnimate(a, npc, "Idle")
				npc.Mu.Lock()
				pauseMs := npc.WanderPauseMinMs
				if npc.WanderPauseMaxMs > npc.WanderPauseMinMs {
					pauseMs += rand.Intn(npc.WanderPauseMaxMs - npc.WanderPauseMinMs + 1)
				}
				if pauseMs > 0 {
					npc.AIMode = AIWanderPause
					npc.WaypointPauseUntil = now + int64(pauseMs)
				} else {
					pickWanderTarget(npc)
				}
				npc.Mu.Unlock()
			} else {
				dist := float32(math.Sqrt(float64(dx*dx + dz*dz)))
				step := float32(npcMoveSpeed * tickSec)
				if step > dist {
					step = dist
				}
				npc.X += (dx / dist) * step
				npc.Z += (dz / dist) * step
				if a.Heightmap != nil {
					npc.Y = a.Heightmap.SampleWorld(npc.X, npc.Z)
				}
				yaw := float32(math.Atan2(float64(dx), float64(dz)) * 180.0 / math.Pi)
				if yaw < 0 {
					yaw += 360
				}
				npc.Yaw = yaw
				broadcastNPCPosition(a, npc)
				BroadcastAnimate(a, npc, "Walk")
			}

		case AIWanderPause:
			if npc.Aggressiveness == 2 {
				a.lookForTarget(npc, players)
				npc.Mu.Lock()
				mode = npc.AIMode
				npc.Mu.Unlock()
				if mode == AIChase {
					continue
				}
			}
			if now >= npc.WaypointPauseUntil {
				npc.Mu.Lock()
				pickWanderTarget(npc)
				npc.AIMode = AIWander
				npc.Mu.Unlock()
			}

		case AIPatrol:
			// Aggressive NPCs interrupt patrol when a player enters range.
			if npc.Aggressiveness == 2 {
				a.lookForTarget(npc, players)
				npc.Mu.Lock()
				mode = npc.AIMode
				npc.Mu.Unlock()
				if mode == AIChase {
					continue
				}
			}
			wp, ok := a.Waypoints[npc.CurrentWaypointID]
			if !ok {
				npc.Mu.Lock()
				npc.AIMode = AIWait
				npc.Mu.Unlock()
				continue
			}
			dx := wp.X - npc.X
			dz := wp.Z - npc.Z
			if dx*dx+dz*dz < 0.25 { // arrived (within 0.5 units)
				npc.X = wp.X
				npc.Z = wp.Z
				broadcastNPCPosition(a, npc)
				nextID := wp.NextA
				if wp.NextB > 0 && rand.Intn(2) == 0 {
					nextID = wp.NextB
				}
				npc.Mu.Lock()
				if wp.PauseMs > 0 {
					npc.AIMode = AIPatrolPause
					npc.WaypointPauseUntil = now + int64(wp.PauseMs)
					// CurrentWaypointID stays so we can re-read NextA/NextB after pause.
				} else if nextID > 0 {
					npc.CurrentWaypointID = nextID
				} else {
					npc.AIMode = AIWait
				}
				npc.Mu.Unlock()
				BroadcastAnimate(a, npc, "Idle")
			} else {
				dx2 := wp.X - npc.X
				dz2 := wp.Z - npc.Z
				dist := float32(math.Sqrt(float64(dx2*dx2 + dz2*dz2)))
				step := float32(npcMoveSpeed * tickSec)
				if step > dist {
					step = dist
				}
				npc.X += (dx2 / dist) * step
				npc.Z += (dz2 / dist) * step
				if a.Heightmap != nil {
					npc.Y = a.Heightmap.SampleWorld(npc.X, npc.Z)
				}
				yaw := float32(math.Atan2(float64(dx2), float64(dz2)) * 180.0 / math.Pi)
				if yaw < 0 {
					yaw += 360
				}
				npc.Yaw = yaw
				broadcastNPCPosition(a, npc)
				BroadcastAnimate(a, npc, "Walk")
			}

		case AIPatrolPause:
			if npc.Aggressiveness == 2 {
				a.lookForTarget(npc, players)
				npc.Mu.Lock()
				mode = npc.AIMode
				npc.Mu.Unlock()
				if mode == AIChase {
					continue
				}
			}
			if now >= npc.WaypointPauseUntil {
				wp, ok := a.Waypoints[npc.CurrentWaypointID]
				nextID := 0
				if ok {
					nextID = wp.NextA
					if wp.NextB > 0 && rand.Intn(2) == 0 {
						nextID = wp.NextB
					}
				}
				npc.Mu.Lock()
				if nextID > 0 {
					npc.CurrentWaypointID = nextID
					npc.AIMode = AIPatrol
				} else {
					npc.AIMode = AIWait
				}
				npc.Mu.Unlock()
			}

		case AIReturn:
			// Aggressive NPCs can re-acquire targets while returning home.
			if npc.Aggressiveness == 2 {
				a.lookForTarget(npc, players)
				npc.Mu.Lock()
				mode = npc.AIMode
				npc.Mu.Unlock()
				if mode == AIChase {
					continue
				}
			}

			dx := npc.SpawnX - npc.X
			dz := npc.SpawnZ - npc.Z
			if dx*dx+dz*dz < 0.25 { // arrived at spawn (within 0.5 units)
				npc.X = npc.SpawnX
				npc.Y = npc.SpawnY
				npc.Z = npc.SpawnZ
				npc.Yaw = npc.SpawnYaw
				broadcastNPCPosition(a, npc)
				npc.Mu.Lock()
				postChaseMode(npc)
				npc.Mu.Unlock()
				BroadcastAnimate(a, npc, "Idle")
			} else {
				moveNPCToPoint(npc, npc.SpawnX, npc.SpawnZ, a, tickSec)
				broadcastNPCPosition(a, npc)
				BroadcastAnimate(a, npc, "Walk")
			}

		case AIChase:
			if target == nil {
				npc.Mu.Lock()
				endChase(npc)
				npc.Mu.Unlock()
				BroadcastAnimate(a, npc, "Idle")
				continue
			}

			// Drop target if it left the area or is already dead.
			if _, ok := a.GetActor(target.RuntimeID); !ok {
				npc.Mu.Lock()
				endChase(npc)
				npc.Mu.Unlock()
				BroadcastAnimate(a, npc, "Idle")
				continue
			}
			target.Mu.Lock()
			targetHP := target.Health
			target.Mu.Unlock()
			if targetHP <= 0 {
				npc.Mu.Lock()
				endChase(npc)
				npc.Mu.Unlock()
				BroadcastAnimate(a, npc, "Idle")
				continue
			}

			// Leash check: if NPC is too far from its spawn point, reset.
			{
				sdx := npc.X - npc.SpawnX
				sdz := npc.Z - npc.SpawnZ
				leash := npc.AggressiveRange * leashMultiple
				if sdx*sdx+sdz*sdz > leash*leash {
					leashNPC(npc, a)
					continue
				}
			}

			// Ability/script decisions are budgeted by npc_combat_profiles.decision_tick_ms.
			// If the budget is closed this tick, NPC keeps normal chase/melee behavior.
			if consumeNPCAbilityDecisionBudget(npc, now) {
				// Optional scripted decision hook runs before built-in special logic.
				if runNPCDecisionHook(a, npc, target, now) {
					continue
				}

				// Special attack/parry timing flow has priority over regular melee.
				if handled, killedBySpecial := ProcessNPCSpecialAttack(a, npc, target, now); handled {
					if killedBySpecial && !target.IsNPC {
						npc.Mu.Lock()
						endChase(npc)
						npc.Mu.Unlock()
						BroadcastAnimate(a, npc, "Idle")
					}
					continue
				}
			}

			// Either attack (if in range) or move closer.
			if InAttackRange(npc, target) {
				dmg, isCrit, onCD, result := ProcessAttack(npc, target)
				if !onCD {
					breakNPCSpecialChain(npc)
					died := BroadcastAttack(a, npc, target, dmg, isCrit, result)
					if died && !target.IsNPC {
						// Player died — clear NPC target, don't add to kill queue.
						npc.Mu.Lock()
						endChase(npc)
						npc.Mu.Unlock()
						BroadcastAnimate(a, npc, "Idle")
					}
				}
			} else {
				// Not in attack range — step toward the target.
				moveNPCToward(npc, target, a, tickSec)
				broadcastNPCPosition(a, npc)
				BroadcastAnimate(a, npc, "Walk")
			}
		}
	}

	// Resolve pending special windups/impacts for players. Target lookup
	// and missing/dead-target cancellation now live inside
	// resolvePendingImpactEntry (called via ResolveActorPendingImpacts) —
	// it resolves each entry against ITS OWN TargetRID, so this loop no
	// longer needs to pre-fetch a single target itself.
	for _, player := range players {
		player.Mu.Lock()
		hasPending := len(player.PendingImpacts) > 0
		player.Mu.Unlock()
		if !hasPending {
			continue
		}
		ResolveActorPendingImpacts(a, player, now)
	}
	_ = toKill
}

// lookForTarget makes an aggressive NPC scan for player targets.
func (a *Area) lookForTarget(npc *Actor, players []*Actor) {
	for _, p := range players {
		p.Mu.Lock()
		hp := p.Health
		p.Mu.Unlock()
		if hp <= 0 {
			continue
		}
		dx := npc.X - p.X
		dz := npc.Z - p.Z
		distSq := dx*dx + dz*dz
		rng := npc.AggressiveRange
		if distSq <= rng*rng {
			npc.Mu.Lock()
			npc.AITarget = p
			npc.AIMode = AIChase
			npc.Mu.Unlock()
			return
		}
	}
}

// KillNPC removes a dead NPC from the area and queues it for respawn.
func (a *Area) KillNPC(npc *Actor) {
	a.RemoveActor(npc.RuntimeID)

	// Broadcast PActorGone so clients remove the NPC.
	var p pb
	p.u32(npc.RuntimeID)
	a.BroadcastAll(buildFrame(pActorGone, p))

	if npc.RespawnDelay > 0 {
		a.dnmu.Lock()
		a.deadNPCs = append(a.deadNPCs, deadNPC{actor: npc, DeadAt: time.Now().UnixMilli()})
		a.dnmu.Unlock()
	}
}

// ---------------------------------------------------------------------------
// Dropped items
// ---------------------------------------------------------------------------

// AddDroppedItem inserts a world item and broadcasts PWorldItem to all players.
func (a *Area) AddDroppedItem(item *DroppedItem) {
	a.diMu.Lock()
	a.droppedItems[item.RuntimeID] = item
	a.diMu.Unlock()

	var p pb
	p.u32(item.RuntimeID)
	p.f32(item.X)
	p.f32(item.Y)
	p.f32(item.Z)
	p.u16(item.ItemID)
	p.u8(item.Quantity)
	p.str(item.Name)
	p.u8(item.ItemType)
	p.str(GetDropModelPath())
	p.f32(GetDropModelScale())
	a.BroadcastAll(buildFrame(pWorldItem, p))
}

// RemoveDroppedItem deletes a world item and broadcasts PRemoveWorldItem.
// Returns false if the item was not found (already picked up).
func (a *Area) RemoveDroppedItem(rid uint32) bool {
	a.diMu.Lock()
	_, ok := a.droppedItems[rid]
	if ok {
		delete(a.droppedItems, rid)
	}
	a.diMu.Unlock()
	if !ok {
		return false
	}
	var p pb
	p.u32(rid)
	a.BroadcastAll(buildFrame(pRemoveWorldItem, p))
	return true
}

// GetDroppedItem returns the dropped item with the given RuntimeID.
func (a *Area) GetDroppedItem(rid uint32) (*DroppedItem, bool) {
	a.diMu.Lock()
	defer a.diMu.Unlock()
	item, ok := a.droppedItems[rid]
	return item, ok
}

// SnapshotDroppedItems returns a copy of all dropped items.
func (a *Area) SnapshotDroppedItems() []*DroppedItem {
	a.diMu.Lock()
	defer a.diMu.Unlock()
	out := make([]*DroppedItem, 0, len(a.droppedItems))
	for _, item := range a.droppedItems {
		out = append(out, item)
	}
	return out
}

// ---------------------------------------------------------------------------
// World object runtime overrides (transform/visibility/collision)
// ---------------------------------------------------------------------------

// findWorldObject returns a pointer into a.Objects for the given ID, or nil.
func (a *Area) findWorldObject(id int) *WorldObject {
	a.Mu.RLock()
	defer a.Mu.RUnlock()
	for i := range a.Objects {
		if a.Objects[i].ID == id {
			return &a.Objects[i]
		}
	}
	return nil
}

// FindWorldObject returns the placed WorldObject with the given ID, if any.
// Used by handleObjectInteract (mirrors Area.GetActor for NPCs).
func (a *Area) FindWorldObject(id int) (*WorldObject, bool) {
	obj := a.findWorldObject(id)
	return obj, obj != nil
}

// baseWorldObjectState builds the default override state for an object still
// at its static authored transform (i.e. not yet present in objectOverrides).
func baseWorldObjectState(o *WorldObject) *WorldObjectState {
	return &WorldObjectState{
		X: o.X, Y: o.Y, Z: o.Z, Yaw: o.Yaw, Pitch: o.Pitch, Roll: o.Roll,
		Scale: o.Scale, Visible: true, Collision: true,
	}
}

// CurrentWorldObjectState returns the object's current state — its active
// override if one exists, otherwise its static authored default (with
// Visible/Collision defaulted to true). Returns false if id does not match
// any object in the area. Used by the Lua World API to read back fields
// (e.g. Scale) not exposed by World.SetTransform's own arguments.
func (a *Area) CurrentWorldObjectState(id int) (WorldObjectState, bool) {
	obj := a.findWorldObject(id)
	if obj == nil {
		return WorldObjectState{}, false
	}
	a.woMu.Lock()
	defer a.woMu.Unlock()
	if st, ok := a.objectOverrides[id]; ok {
		return *st, true
	}
	return *baseWorldObjectState(obj), true
}

// GetWorldObjectOverride returns the current override state for the given
// object ID, if it has ever diverged from its static authored default.
func (a *Area) GetWorldObjectOverride(id int) (*WorldObjectState, bool) {
	a.woMu.Lock()
	defer a.woMu.Unlock()
	st, ok := a.objectOverrides[id]
	return st, ok
}

// SnapshotWorldObjectOverrides returns a copy of the override map, used to
// merge overrides into the PWorldObjects snapshot sent on area entry.
func (a *Area) SnapshotWorldObjectOverrides() map[int]*WorldObjectState {
	a.woMu.Lock()
	defer a.woMu.Unlock()
	out := make(map[int]*WorldObjectState, len(a.objectOverrides))
	for id, st := range a.objectOverrides {
		out[id] = st
	}
	return out
}

// broadcastWorldObjectUpdate sends PWorldObjectUpdate for the object's
// current override state. durationSec is the client-side animation length for
// the transform (0 = instant); visible/collision always apply immediately.
// Pitch/Roll are included so a client's WorldObjectEntry never loses its
// authored orientation on a runtime update (World.SetTransform only ever
// changes X/Y/Z/Yaw — see UpdateWorldObjectTransform — pitch/roll here are
// always whatever the override/base object already had).
func (a *Area) broadcastWorldObjectUpdate(id int, st *WorldObjectState, durationSec float32) {
	var p pb
	p.u32(uint32(id))
	p.f32(st.X)
	p.f32(st.Y)
	p.f32(st.Z)
	p.f32(st.Yaw)
	p.f32(st.Pitch)
	p.f32(st.Roll)
	p.f32(st.Scale)
	p.f32(durationSec)
	p.u8(boolU8(st.Visible))
	p.u8(boolU8(st.Collision))
	a.BroadcastAll(buildFrame(pWorldObjectUpdate, p))
}

// UpdateWorldObjectTransform sets the object's override to the given target
// transform and broadcasts PWorldObjectUpdate so connected clients animate
// toward it over durationSec (0 = instant). The override always stores the
// FINAL target state, not a mid-flight snapshot — an object entering the area
// (or a client loading in) mid-animation is therefore always born already at
// the correct settled state; only clients already in the area play the
// animation. Returns false if id does not match any object in the area.
func (a *Area) UpdateWorldObjectTransform(id int, x, y, z, yaw, scale, durationSec float32) bool {
	obj := a.findWorldObject(id)
	if obj == nil {
		return false
	}

	a.woMu.Lock()
	st, ok := a.objectOverrides[id]
	if !ok {
		st = baseWorldObjectState(obj)
		a.objectOverrides[id] = st
	}
	st.X, st.Y, st.Z, st.Yaw, st.Scale = x, y, z, yaw, scale
	stCopy := *st
	a.woMu.Unlock()

	a.broadcastWorldObjectUpdate(id, &stCopy, durationSec)
	return true
}

// SetWorldObjectVisible sets the object's visibility override and broadcasts
// PWorldObjectUpdate with duration=0 (visibility applies immediately, never
// interpolated). Returns false if id does not match any object in the area.
func (a *Area) SetWorldObjectVisible(id int, visible bool) bool {
	obj := a.findWorldObject(id)
	if obj == nil {
		return false
	}

	a.woMu.Lock()
	st, ok := a.objectOverrides[id]
	if !ok {
		st = baseWorldObjectState(obj)
		a.objectOverrides[id] = st
	}
	st.Visible = visible
	stCopy := *st
	a.woMu.Unlock()

	a.broadcastWorldObjectUpdate(id, &stCopy, 0)
	return true
}

// SetWorldObjectCollision sets the object's collision override and
// broadcasts PWorldObjectUpdate with duration=0 (collision applies
// immediately, never interpolated). Returns false if id does not match any
// object in the area.
func (a *Area) SetWorldObjectCollision(id int, collision bool) bool {
	obj := a.findWorldObject(id)
	if obj == nil {
		return false
	}

	a.woMu.Lock()
	st, ok := a.objectOverrides[id]
	if !ok {
		st = baseWorldObjectState(obj)
		a.objectOverrides[id] = st
	}
	st.Collision = collision
	stCopy := *st
	a.woMu.Unlock()

	a.broadcastWorldObjectUpdate(id, &stCopy, 0)
	return true
}

// SpawnEphemeralObject creates ONE temporary, script-spawned WorldObject
// (Lua World.SpawnTempProp — scripting/api.go) that moves from
// (startX,startY,startZ,startYaw) to (endX,endY,endZ,endYaw) over
// durationMs, then auto-removes itself. Not zone_scenery-backed, never
// persisted — exists only in a.ephemeralObjects and on connected clients
// for its lifetime. Reuses Area.ScheduleCall (the SAME timer mechanism
// Lua's Timer.after already uses, checked every ~100ms via tickAI) for the
// despawn — no new ticker/scan loop needed.
//
// If onArrivalFXKey is non-empty, BroadcastFXAtPosition fires at the END
// transform the moment the object is removed — purely a convenience
// (reuses FX.play's own primitive internally); a script can just as well
// call FX.play itself instead/additionally, the two are not coupled.
//
// Returns the new object's id.
func (a *Area) SpawnEphemeralObject(
	modelPath string,
	scale float32,
	startX, startY, startZ, startYaw float32,
	endX, endY, endZ, endYaw float32,
	durationMs int64,
	onArrivalFXKey string,
) int {
	if a == nil {
		return 0
	}
	if durationMs < 0 {
		durationMs = 0
	}

	obj := &EphemeralObject{
		ID:             NewEphemeralObjectID(),
		ModelPath:      modelPath,
		Scale:          scale,
		StartX:         startX,
		StartY:         startY,
		StartZ:         startZ,
		StartYaw:       startYaw,
		EndX:           endX,
		EndY:           endY,
		EndZ:           endZ,
		EndYaw:         endYaw,
		SpawnedAt:      time.Now().UnixMilli(),
		DurationMs:     durationMs,
		OnArrivalFXKey: onArrivalFXKey,
	}

	a.eoMu.Lock()
	a.ephemeralObjects[obj.ID] = obj
	a.eoMu.Unlock()

	a.BroadcastAll(buildFrame(pWorldObjectSpawn, EphemeralObjectSpawnPayload(obj)))

	id := obj.ID
	a.ScheduleCall(durationMs, func() {
		a.eoMu.Lock()
		delete(a.ephemeralObjects, id)
		a.eoMu.Unlock()

		var p pb
		p.u32(uint32(id))
		a.BroadcastAll(buildFrame(pWorldObjectDespawn, p))

		if onArrivalFXKey != "" {
			BroadcastFXAtPosition(a, onArrivalFXKey, endX, endY, endZ, 1.0)
		}
	})

	return obj.ID
}

// SpawnDropsForNPC rolls the drop table for npc and adds any results to the world.
func (a *Area) SpawnDropsForNPC(npc *Actor) {
	if npc == nil {
		return
	}
	if npc.LootTableID <= 0 {
		return
	}
	drops := RollDropsByTable(npc.LootTableID, npc.X, npc.Y, npc.Z)
	if len(drops) > 0 {
		log.Printf("world: %s dropped %d item(s)", npc.Name, len(drops))
	}
	for _, d := range drops {
		a.AddDroppedItem(d)
	}
}

// tickDropDespawn removes dropped items older than 60 s and broadcasts removal.
func (a *Area) tickDropDespawn(now int64) {
	a.diMu.Lock()
	var expired []uint32
	for rid, item := range a.droppedItems {
		if now-item.SpawnedAt >= 60_000 {
			expired = append(expired, rid)
		}
	}
	for _, rid := range expired {
		delete(a.droppedItems, rid)
	}
	a.diMu.Unlock()

	for _, rid := range expired {
		var p pb
		p.u32(rid)
		a.BroadcastAll(buildFrame(pRemoveWorldItem, p))
	}
}

// ScheduleCall runs fn once, after delayMs have elapsed, on this area's own
// AI tick goroutine (see tickScheduledCalls — checked every ~100ms, so
// firing isn't millisecond-precise but is fine for gameplay timers like
// "close the gate after 20s"). fn is called with no locks held by this
// package; a caller that needs to reenter another subsystem (e.g. Lua) is
// responsible for its own locking, exactly like the scripting package's
// Timer.after binding does.
func (a *Area) ScheduleCall(delayMs int64, fn func()) {
	a.scMu.Lock()
	a.scheduled = append(a.scheduled, scheduledCall{runAt: time.Now().UnixMilli() + delayMs, fn: fn})
	a.scMu.Unlock()
}

// tickScheduledCalls runs (and removes) any scheduled calls whose deadline
// has passed. Mirrors tickDropDespawn: collect due entries under the lock,
// then invoke them after unlocking (so a callback that calls back into other
// Area methods, e.g. UpdateWorldObjectTransform, can't deadlock on scMu).
func (a *Area) tickScheduledCalls(now int64) {
	a.scMu.Lock()
	var due []func()
	remaining := a.scheduled[:0]
	for _, sc := range a.scheduled {
		if now >= sc.runAt {
			due = append(due, sc.fn)
		} else {
			remaining = append(remaining, sc)
		}
	}
	a.scheduled = remaining
	a.scMu.Unlock()

	for _, fn := range due {
		fn()
	}
}

// respawnNPC resets an NPC and adds it back to the area.
func (a *Area) respawnNPC(npc *Actor) {
	npc.Mu.Lock()
	npc.Health = npc.HealthMax
	npc.Stamina = npc.StaminaMax
	npc.DeadAt = 0
	npc.X = npc.SpawnX
	npc.Y = npc.SpawnY
	npc.Z = npc.SpawnZ
	npc.Yaw = npc.SpawnYaw
	npc.Guarding = false
	npc.GuardUntil = 0
	npc.ParryUntil = 0
	npc.DodgeUntil = 0
	npc.PendingImpacts = nil
	npc.SpecialChainCount = 0
	npc.AbilityCooldowns = make(map[int]int64)
	postChaseMode(npc)
	npc.Mu.Unlock()

	a.AddActor(npc)

	// Broadcast PNewActor so clients see the NPC again.
	a.BroadcastAll(buildFrame(pNewActor, NewActorPayload(npc)))
}
