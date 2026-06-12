// Package world - combat_melee.go
//
// Melee attack processing and the attack broadcast flow.
package world

import (
	"math/rand"
	"time"
)

// InMeleeRange returns true if a1 is close enough to hit a2.
// Uses a1.AttackRange if set; falls back to the MeleeRange constant.
func InMeleeRange(a1, a2 *Actor) bool {
	dx := a1.X - a2.X
	dz := a1.Z - a2.Z
	dy := (a1.Y - a2.Y) / 5.0
	distSq := dx*dx + dz*dz + dy*dy
	base := a1.AttackRange
	if base == 0 {
		base = MeleeRange
	}
	max := base + a1.Radius + a2.Radius
	return distSq <= max*max
}

// ProcessAttack executes one melee attack from attacker -> target.
// Returns (damage, isCrit, onCooldown, result).
// damage == -1 means miss or fully avoided hit.
func ProcessAttack(attacker, target *Actor) (damage int32, isCrit bool, onCooldown bool, result AttackResult) {
	result = AttackResultNormal

	attacker.Mu.Lock()
	isGuarding := attacker.Guarding
	attacker.Mu.Unlock()
	if isGuarding {
		return 0, false, true, AttackResultNormal
	}

	// Enforce attack cooldown under attacker lock.
	attacker.Mu.Lock()
	now := time.Now().UnixMilli()
	if now-attacker.LastAttack < CombatDelay {
		attacker.Mu.Unlock()
		return 0, false, true, result
	}
	attacker.LastAttack = now
	attacker.LastCombatAt = now
	aDerived := attacker.Derived
	attacker.Mu.Unlock()

	target.Mu.Lock()
	tDerived := target.Derived
	legacyArmor := target.CachedArmor
	target.Mu.Unlock()

	// Comparative hit check (attacker hit vs target evasion).
	hitChance := computeHitChance(aDerived.MeleeHitValue, tDerived.MeleeEvasionValue)
	if rand.Float32() > hitChance {
		return -1, false, false, AttackResultMiss
	}

	// Damage range from Derived melee stats.
	minDmg := aDerived.MeleeDmgMin
	maxDmg := aDerived.MeleeDmgMax
	if maxDmg < minDmg {
		maxDmg = minDmg
	}
	dmg := minDmg
	if maxDmg > minDmg {
		dmg += rand.Int31n(maxDmg - minDmg + 1)
	}

	// Defense reduction from target Derived defense curve.
	defPct := ValueToPercent(tDerived.MeleeDefenseValue, defenseCap, defenseSoftcap)
	dmg = int32(float32(dmg) * (1.0 - defPct))
	// Flat reduction from target Derived.
	dmg -= tDerived.DamageReductionFlat
	// Legacy armor reduction remains as an additional reduction layer.
	dmg -= legacyArmor
	if dmg < 1 {
		dmg = 1
	}

	// Critical hit from Derived crit value and multiplier.
	critPct := ValueToPercent(aDerived.MeleeCritValue, critValueCap, critValueSoftcap)
	if rand.Float32() < critPct {
		critMult := aDerived.CritDamageMult
		if critMult < 1 {
			critMult = 1
		}
		dmg = int32(float32(dmg) * critMult)
		if dmg < 1 {
			dmg = 1
		}
		isCrit = true
	}

	// Defensive reactions and damage application under target lock.
	now2 := time.Now().UnixMilli()
	target.Mu.Lock()
	defer target.Mu.Unlock()
	if target.DodgeUntil > now2 {
		return -1, false, false, AttackResultDodged
	}
	if target.ParryUntil > now2 {
		target.ParryUntil = 0
		return -1, false, false, AttackResultParried
	}
	if target.Guarding && target.Stamina > 0 {
		dmg = (dmg*guardDamagePct + 99) / 100 // ceil(dmg * pct / 100)
		if dmg < 1 {
			dmg = 1
		}
		target.Stamina -= guardHitSPCost
		if target.Stamina <= 0 {
			target.Stamina = 0
			target.Guarding = false
			target.GuardUntil = 0
		}
		result = AttackResultGuarded
	}
	target.Health -= dmg
	return dmg, isCrit, false, result
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
func BroadcastAttack(area *Area, attacker, target *Actor, damage int32, isCrit bool, result AttackResult) bool {
	dmgType := uint8(0) // physical

	BroadcastAnimate(area, attacker, "Attack")

	// "H" packet -> attacker (if player).
	if !attacker.IsNPC {
		var p pb
		p.u8('H')
		p.u32(target.RuntimeID)
		p.u16(uint16(damage + 1)) // +1 so 0 = miss (RC convention)
		p.u8(dmgType)
		p.u8(boolU8(isCrit))
		attacker.Send(buildFrame(pAttackActor, p))
	}

	// "Y" packet -> target (if player).
	if !target.IsNPC {
		var p pb
		p.u8('Y')
		p.u32(attacker.RuntimeID)
		p.u16(uint16(damage + 1))
		p.u8(dmgType)
		p.u8(boolU8(isCrit))
		target.Send(buildFrame(pAttackActor, p))
	}

	// "O" packet -> all other players.
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

	// PFloatingNumber -> all players.
	sendFloating := damage >= 0 || result == AttackResultMiss
	if sendFloating {
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

	switch result {
	case AttackResultDodged:
		BroadcastCombatEvent(area, combatEventHitDodged, target.RuntimeID, attacker.RuntimeID, 0, "")
	case AttackResultGuarded:
		BroadcastCombatEvent(area, combatEventHitGuarded, target.RuntimeID, attacker.RuntimeID, int16(damage), "")
	case AttackResultParried:
		BroadcastCombatEvent(area, combatEventHitParried, target.RuntimeID, attacker.RuntimeID, 0, "")
	}
	if isCrit && damage > 0 {
		BroadcastCombatEvent(area, combatEventCritHit, attacker.RuntimeID, target.RuntimeID, int16(damage), "")
	}

	// Broadcast "Hit" animation on the target if it has that action.
	// Only when damage was actually dealt (not a miss).
	if damage > 0 {
		BroadcastAnimate(area, target, "Hit")
	}

	// PStatUpdate -> target if it's a player.
	target.Mu.Lock()
	hp := target.Health
	sp := target.Stamina
	guardBroken := result == AttackResultGuarded && sp == 0
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
	if !target.IsNPC && result == AttackResultGuarded {
		BroadcastSPUpdate(target, sp)
	}
	if guardBroken {
		BroadcastCombatEvent(area, combatEventGuardEnded, target.RuntimeID, 0, 0, "Out of stamina")
	}

	if dead {
		BroadcastAnimate(area, target, "Death")
		// PActorDead -> all players.
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

func computeHitChance(hit, evasion int32) float32 {
	total := hit + evasion
	if total <= 0 {
		return 0.95
	}
	chance := float32(hit) / float32(total)
	if chance < 0.10 {
		return 0.10
	}
	if chance > 0.95 {
		return 0.95
	}
	return chance
}
