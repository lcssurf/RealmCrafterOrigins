package world

import (
	"context"
	"log"
	"math"
	"math/rand"
	"sync"
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
	ModelPath string
	Scale     float32
	X, Y, Z   float32
	Yaw       float32
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

func (a *Area) tickRegen() {
	now := time.Now().UnixMilli()
	actors := a.Snapshot()
	for _, actor := range actors {
		actor.Mu.Lock()
		dead := actor.DeadAt > 0
		inCombat := actor.LastCombatAt > 0 && now-actor.LastCombatAt < regenCombatWindow
		hp, hpMax := actor.Health, actor.HealthMax
		mp, mpMax := actor.Energy, actor.EnergyMax
		sp, spMax := actor.Stamina, actor.StaminaMax
		actor.Mu.Unlock()

		if dead {
			continue
		}

		var hpGain, mpGain, spGain int32
		if !inCombat {
			if hp < hpMax {
				hpGain = hpMax / 20 // 5% per tick
				if hpGain < 1 {
					hpGain = 1
				}
			}
			if mp < mpMax {
				mpGain = mpMax / 12 // ~8% per tick
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
		ticker := time.NewTicker(500 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				a.tickAI()
			}
		}
	}()
}

const (
	npcMoveSpeed  = 5.0 // world units per second
	aiTickSec     = 0.5 // seconds per AI tick (matches 500ms ticker)
	leashMultiple = 2.5 // NPC leashes when player is > aggro * leashMultiple from NPC spawn
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
func moveNPCToward(npc, target *Actor, a *Area) {
	dx := target.X - npc.X
	dz := target.Z - npc.Z
	dist := float32(math.Sqrt(float64(dx*dx + dz*dz)))
	if dist < 0.05 {
		return
	}
	step := float32(npcMoveSpeed * aiTickSec)
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

// leashNPC resets an NPC to its spawn point and resumes the appropriate idle
// mode (patrol, wander, or stationary).
func leashNPC(npc *Actor, a *Area) {
	npc.Mu.Lock()
	npc.X = npc.SpawnX
	npc.Y = npc.SpawnY
	npc.Z = npc.SpawnZ
	npc.Yaw = npc.SpawnYaw
	npc.Guarding = false
	npc.GuardUntil = 0
	npc.ParryUntil = 0
	npc.DodgeUntil = 0
	npc.SpecialWindupUntil = 0
	npc.SpecialTargetRID = 0
	npc.SpecialAbilityID = 0
	npc.SpecialActionOverride = ""
	npc.SpecialReasonTag = ""
	npc.SpecialClientTraceID = ""
	npc.SpecialChainCount = 0
	npc.AbilityCooldowns = make(map[int]int64)
	postChaseMode(npc)
	npc.Mu.Unlock()
	broadcastNPCPosition(a, npc)
	BroadcastAnimate(a, npc, "Idle")
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
	npc.SpecialWindupUntil = 0
	npc.SpecialTargetRID = 0
	npc.SpecialAbilityID = 0
	npc.SpecialActionOverride = ""
	npc.SpecialReasonTag = ""
	npc.SpecialClientTraceID = ""
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
func (a *Area) tickAI() {
	now := time.Now().UnixMilli()
	a.tickDropDespawn(now)

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
				step := float32(npcMoveSpeed * aiTickSec)
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
				step := float32(npcMoveSpeed * aiTickSec)
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
			if InMeleeRange(npc, target) {
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
				moveNPCToward(npc, target, a)
				broadcastNPCPosition(a, npc)
				BroadcastAnimate(a, npc, "Walk")
			}
		}
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

// SpawnDropsForNPC rolls the drop table for npc and adds any results to the world.
func (a *Area) SpawnDropsForNPC(npc *Actor) {
	drops := RollDrops(npc.Name, npc.X, npc.Y, npc.Z)
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
	npc.SpecialWindupUntil = 0
	npc.SpecialTargetRID = 0
	npc.SpecialAbilityID = 0
	npc.SpecialActionOverride = ""
	npc.SpecialReasonTag = ""
	npc.SpecialClientTraceID = ""
	npc.SpecialChainCount = 0
	npc.AbilityCooldowns = make(map[int]int64)
	postChaseMode(npc)
	npc.Mu.Unlock()

	a.AddActor(npc)

	// Broadcast PNewActor so clients see the NPC again.
	a.BroadcastAll(buildFrame(pNewActor, NewActorPayload(npc)))
}
