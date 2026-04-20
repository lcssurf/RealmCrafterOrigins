package world

import (
	"math/rand"
	"time"
)

const (
	CombatDelay = 800 // ms between attacks
	MeleeRange  = 7.0 // world units
)

// Packet type constants mirrored from protocol (avoids circular import).
const (
	pAttackActor  uint16 = 18
	pActorDead    uint16 = 19
	pStatUpdate   uint16 = 22
	pNewActor     uint16 = 11
	pActorGone    uint16 = 13
	pFloatingNum  uint16 = 48
)

// InMeleeRange returns true if a1 is close enough to hit a2 in melee.
func InMeleeRange(a1, a2 *Actor) bool {
	dx := a1.X - a2.X
	dz := a1.Z - a2.Z
	dy := (a1.Y - a2.Y) / 5.0
	distSq := dx*dx + dz*dz + dy*dy
	max := MeleeRange + a1.Radius + a2.Radius
	return distSq <= max*max
}

// ProcessAttack executes one melee attack from attacker → target.
// Returns (damage, isCrit, onCooldown).
// damage == -1 means miss; damage == 0 means hit but no damage (blocked by armor).
func ProcessAttack(attacker, target *Actor) (damage int32, isCrit bool, onCooldown bool) {
	// Enforce attack cooldown under attacker lock.
	attacker.Mu.Lock()
	now := time.Now().UnixMilli()
	if now-attacker.LastAttack < CombatDelay {
		attacker.Mu.Unlock()
		return 0, false, true
	}
	attacker.LastAttack = now
	attacker.LastCombatAt = now
	attacker.Mu.Unlock()

	// 90% hit chance (RC formula 1).
	if rand.Intn(100) < 10 {
		return -1, false, false
	}

	// Base damage from weapon + strength modifier.
	strength := attacker.Strength
	wdmg := attacker.WeaponDamage
	var dmg int32
	if wdmg == 0 {
		// Unarmed: fist damage from strength.
		dmg = strength/8 + rand.Int31n(11) - 5
	} else {
		dmg = wdmg
		if strength < wdmg {
			dmg -= rand.Int31n(4) + 5
		} else if strength > wdmg {
			dmg += rand.Int31n(4) + 5
		} else {
			dmg += rand.Int31n(11) - 5
		}
	}

	// Critical hit: 10% chance, doubles damage.
	if rand.Intn(10) == 0 {
		dmg *= 2
		isCrit = true
	}

	// Armor reduction (target's cached armor sum).
	dmg -= target.CachedArmor
	if dmg < 1 {
		dmg = 1
	}

	// Apply damage.
	target.Mu.Lock()
	target.Health -= dmg
	target.Mu.Unlock()

	return dmg, isCrit, false
}

// BroadcastAttack sends all combat-related packets for one hit:
//   - "H" (hit) to attacker if it's a player.
//   - "Y" (you were hit) to target if it's a player.
//   - "O" (observer) to all other players in the area.
//   - PFloatingNumber to all players in the area.
//   - PStatUpdate to the target if it's a player.
//   - PActorDead to all if the target died.
//
// Returns true if the target died.
func BroadcastAttack(area *Area, attacker, target *Actor, damage int32, isCrit bool) bool {
	dmgType := uint8(0) // physical

	// "H" packet → attacker (if player).
	if !attacker.IsNPC {
		var p pb
		p.u8('H')
		p.u32(target.RuntimeID)
		p.u16(uint16(damage + 1)) // +1 so 0 = miss (RC convention)
		p.u8(dmgType)
		p.u8(boolU8(isCrit))
		attacker.Send(buildFrame(pAttackActor, p))
	}

	// "Y" packet → target (if player).
	if !target.IsNPC {
		var p pb
		p.u8('Y')
		p.u32(attacker.RuntimeID)
		p.u16(uint16(damage + 1))
		p.u8(dmgType)
		p.u8(boolU8(isCrit))
		target.Send(buildFrame(pAttackActor, p))
	}

	// "O" packet → all other players.
	{
		var p pb
		p.u8('O')
		p.u32(attacker.RuntimeID)
		p.u32(target.RuntimeID)
		frame := buildFrame(pAttackActor, p)
		area.Mu.RLock()
		for _, a := range area.actors {
			if a.IsNPC || a == attacker || a == target {
				continue
			}
			a.Send(frame)
		}
		area.Mu.RUnlock()
	}

	// PFloatingNumber → all players.
	{
		var p pb
		p.u32(target.RuntimeID)
		if damage == -1 {
			p.i16(-1) // miss
		} else {
			p.i16(int16(damage))
		}
		p.u8(boolU8(isCrit))
		frame := buildFrame(pFloatingNum, p)
		area.Mu.RLock()
		for _, a := range area.actors {
			if !a.IsNPC {
				a.Send(frame)
			}
		}
		area.Mu.RUnlock()
	}

	// PStatUpdate → target if it's a player.
	target.Mu.Lock()
	hp := target.Health
	dead := hp <= 0 && target.DeadAt == 0
	now2 := time.Now().UnixMilli()
	if dead {
		target.DeadAt = now2
	}
	target.LastCombatAt = now2
	target.Mu.Unlock()

	{
		var p pb
		p.u8('A')
		p.u32(target.RuntimeID)
		p.u8(0) // attr 0 = HP
		p.i16(int16(hp))
		frame := buildFrame(pStatUpdate, p)
		if target.IsNPC {
			// Broadcast NPC HP to all players so their target bars update.
			area.Mu.RLock()
			for _, a := range area.actors {
				if !a.IsNPC {
					a.Send(frame)
				}
			}
			area.Mu.RUnlock()
		} else {
			target.Send(frame)
		}
	}

	if dead {
		// PActorDead → all players.
		var p pb
		p.u32(target.RuntimeID)
		p.u32(attacker.RuntimeID)
		frame := buildFrame(pActorDead, p)
		area.Mu.RLock()
		for _, a := range area.actors {
			if !a.IsNPC {
				a.Send(frame)
			}
		}
		area.Mu.RUnlock()
	}

	return dead
}

func boolU8(v bool) uint8 {
	if v {
		return 1
	}
	return 0
}
