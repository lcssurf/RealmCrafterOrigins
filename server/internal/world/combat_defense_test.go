package world

import (
	"testing"
	"time"
)

func newCombatActor(name string) *Actor {
	a := NewActor()
	a.Name = name
	a.Level = 10
	a.Health = 200
	a.HealthMax = 200
	a.Energy = 100
	a.EnergyMax = 100
	a.Stamina = 100
	a.StaminaMax = 100
	a.Strength = 40
	a.WeaponDamage = 30
	return a
}

func runAttackUntilNotMiss(t *testing.T, attacker, target *Actor) (int32, AttackResult) {
	t.Helper()
	for i := 0; i < 128; i++ {
		attacker.Mu.Lock()
		attacker.LastAttack = 0
		attacker.Mu.Unlock()

		dmg, _, onCD, result := ProcessAttack(attacker, target)
		if onCD {
			continue
		}
		if result == AttackResultMiss {
			continue
		}
		return dmg, result
	}
	t.Fatalf("could not resolve attack outcome without miss after many tries")
	return 0, AttackResultMiss
}

func TestProcessAttackDodgeAvoidsDamage(t *testing.T) {
	attacker := newCombatActor("Attacker")
	target := newCombatActor("Target")
	target.DodgeUntil = time.Now().UnixMilli() + 5_000
	startHP := target.Health

	dmg, result := runAttackUntilNotMiss(t, attacker, target)
	if result != AttackResultDodged {
		t.Fatalf("unexpected attack result: got=%d want=%d", result, AttackResultDodged)
	}
	if dmg != -1 {
		t.Fatalf("dodge should mark attack as avoided: got damage=%d want=-1", dmg)
	}
	if target.Health != startHP {
		t.Fatalf("target HP should stay the same on dodge: got=%d want=%d", target.Health, startHP)
	}
}

func TestProcessAttackParryAvoidsDamage(t *testing.T) {
	attacker := newCombatActor("Attacker")
	target := newCombatActor("Target")
	target.ParryUntil = time.Now().UnixMilli() + 5_000
	startHP := target.Health

	dmg, result := runAttackUntilNotMiss(t, attacker, target)
	if result != AttackResultParried {
		t.Fatalf("unexpected attack result: got=%d want=%d", result, AttackResultParried)
	}
	if dmg != -1 {
		t.Fatalf("parry should mark attack as avoided: got damage=%d want=-1", dmg)
	}
	if target.Health != startHP {
		t.Fatalf("target HP should stay the same on parry: got=%d want=%d", target.Health, startHP)
	}
}

func TestProcessAttackGuardMitigatesAndConsumesSP(t *testing.T) {
	attacker := newCombatActor("Attacker")
	target := newCombatActor("Target")
	target.Guarding = true
	target.GuardUntil = time.Now().UnixMilli() + 5_000
	startHP := target.Health
	startSP := target.Stamina

	dmg, result := runAttackUntilNotMiss(t, attacker, target)
	if result != AttackResultGuarded {
		t.Fatalf("unexpected attack result: got=%d want=%d", result, AttackResultGuarded)
	}
	if dmg <= 0 {
		t.Fatalf("guarded hit should still deal reduced positive damage, got=%d", dmg)
	}
	if target.Health >= startHP {
		t.Fatalf("guarded hit should reduce HP: start=%d got=%d", startHP, target.Health)
	}
	if target.Stamina >= startSP {
		t.Fatalf("guarded hit should consume SP: start=%d got=%d", startSP, target.Stamina)
	}
}
