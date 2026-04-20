package world

import (
	"context"
	"sync"
	"time"
)

type deadNPC struct {
	actor  *Actor
	DeadAt int64 // unix ms
}

// Area represents a game zone.
type Area struct {
	Name    string
	actors  map[uint32]*Actor
	Mu      sync.RWMutex
	Portals []Portal

	// Dead NPCs waiting to respawn.
	dnmu     sync.Mutex
	deadNPCs []deadNPC
}

// NewArea creates an empty Area.
func NewArea(name string) *Area {
	return &Area{
		Name:   name,
		actors: make(map[uint32]*Actor),
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

// StartRegen launches the out-of-combat HP/EP regeneration goroutine for this area.
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
		ep, epMax := actor.Energy, actor.EnergyMax
		actor.Mu.Unlock()

		if dead || inCombat {
			continue
		}

		var hpGain, epGain int32
		if hp < hpMax {
			hpGain = hpMax / 20 // 5% per tick
			if hpGain < 1 {
				hpGain = 1
			}
		}
		if ep < epMax {
			epGain = epMax / 12 // ~8% per tick
			if epGain < 1 {
				epGain = 1
			}
		}
		if hpGain == 0 && epGain == 0 {
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
		if epGain > 0 {
			actor.Energy += epGain
			if actor.Energy > actor.EnergyMax {
				actor.Energy = actor.EnergyMax
			}
			ep = actor.Energy
		}
		actor.Mu.Unlock()

		BroadcastHPUpdate(a, actor, hp)
		if !actor.IsNPC && epGain > 0 {
			BroadcastEPUpdate(actor, ep)
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

// tickAI runs one AI update step for all NPCs in the area.
func (a *Area) tickAI() {
	now := time.Now().UnixMilli()

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
			if npc.Aggressiveness == 2 {
				a.lookForTarget(npc, players)
			}

		case AIChase:
			if target == nil {
				npc.Mu.Lock()
				npc.AIMode = AIWait
				npc.Mu.Unlock()
				continue
			}

			// Drop target if it left the area or is already dead.
			if _, ok := a.GetActor(target.RuntimeID); !ok {
				npc.Mu.Lock()
				npc.AITarget = nil
				npc.AIMode = AIWait
				npc.Mu.Unlock()
				continue
			}
			target.Mu.Lock()
			targetHP := target.Health
			target.Mu.Unlock()
			if targetHP <= 0 {
				npc.Mu.Lock()
				npc.AITarget = nil
				npc.AIMode = AIWait
				npc.Mu.Unlock()
				continue
			}

			// Attack if in range.
			if InMeleeRange(npc, target) {
				dmg, isCrit, onCD := ProcessAttack(npc, target)
				if !onCD {
					died := BroadcastAttack(a, npc, target, dmg, isCrit)
					if died && !target.IsNPC {
						// Player died — clear NPC target, don't add to kill queue.
						npc.Mu.Lock()
						npc.AITarget = nil
						npc.AIMode = AIWait
						npc.Mu.Unlock()
					}
				}
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

// respawnNPC resets an NPC and adds it back to the area.
func (a *Area) respawnNPC(npc *Actor) {
	npc.Mu.Lock()
	npc.Health = npc.HealthMax
	npc.DeadAt = 0
	npc.AIMode = AIWait
	npc.AITarget = nil
	npc.X = npc.SpawnX
	npc.Y = npc.SpawnY
	npc.Z = npc.SpawnZ
	npc.Yaw = npc.SpawnYaw
	npc.Mu.Unlock()

	a.AddActor(npc)

	// Broadcast PNewActor so clients see the NPC again.
	a.BroadcastAll(buildFrame(pNewActor, newActorPayload(npc)))
}
