package world

import "time"

// ApplyDamage reduces target HP by amount. Returns (newHP, justDied).
// Sets target.DeadAt if this is the killing blow.
func ApplyDamage(target *Actor, amount int32, attackerRID uint32) (hp int32, justDied bool) {
	target.Mu.Lock()
	defer target.Mu.Unlock()
	now := time.Now().UnixMilli()
	target.Health -= amount
	target.LastCombatAt = now
	hp = target.Health
	justDied = hp <= 0 && target.DeadAt == 0
	if justDied {
		target.DeadAt = now
	}
	return hp, justDied
}

// ApplyHeal adds amount to actor HP, capped at HealthMax. Returns new HP.
func ApplyHeal(actor *Actor, amount int32) int32 {
	actor.Mu.Lock()
	defer actor.Mu.Unlock()
	actor.Health += amount
	if actor.Health > actor.HealthMax {
		actor.Health = actor.HealthMax
	}
	return actor.Health
}

// BroadcastFloatingNumber sends a floating damage/heal number to all players in the area.
// dmgType: 0=physical, 1=magic/heal.
func BroadcastFloatingNumber(area *Area, onActor *Actor, value int16, dmgType uint8) {
	var p pb
	p.u32(onActor.RuntimeID)
	p.i16(value)
	p.u8(dmgType)
	frame := buildFrame(pFloatingNum, p)
	area.Mu.RLock()
	for _, a := range area.actors {
		if !a.IsNPC {
			a.Send(frame)
		}
	}
	area.Mu.RUnlock()
}

// BroadcastHPUpdate sends PStatUpdate(attr=HP) for the given actor to the relevant recipients.
func BroadcastHPUpdate(area *Area, actor *Actor, hp int32) {
	var p pb
	p.u8('A')
	p.u32(actor.RuntimeID)
	p.u8(0) // attr 0 = HP
	p.i16(int16(hp))
	frame := buildFrame(pStatUpdate, p)
	if actor.IsNPC {
		area.Mu.RLock()
		for _, a := range area.actors {
			if !a.IsNPC {
				a.Send(frame)
			}
		}
		area.Mu.RUnlock()
	} else {
		actor.Send(frame)
	}
}

// BroadcastEPUpdate sends PStatUpdate(attr=EP) to the actor's own client.
func BroadcastEPUpdate(actor *Actor, ep int32) {
	var p pb
	p.u8('A')
	p.u32(actor.RuntimeID)
	p.u8(2) // attr 2 = EP
	p.i16(int16(ep))
	actor.Send(buildFrame(pStatUpdate, p))
}

// BroadcastActorDead sends PActorDead to all players in the area.
func BroadcastActorDead(area *Area, targetRID, killerRID uint32) {
	var p pb
	p.u32(targetRID)
	p.u32(killerRID)
	frame := buildFrame(pActorDead, p)
	area.Mu.RLock()
	for _, a := range area.actors {
		if !a.IsNPC {
			a.Send(frame)
		}
	}
	area.Mu.RUnlock()
}
