package world

import (
	"testing"
	"time"
)

func TestProcessNPCSpecialAttackParryExactTiming(t *testing.T) {
	area := NewArea("Test Area")
	npc := newCombatActor("Mob")
	target := newCombatActor("Player")

	npc.RuntimeID = 1001
	npc.IsNPC = true
	npc.X, npc.Z = 0, 0
	npc.Radius = 0.4
	npc.AttackRange = 2.0

	target.RuntimeID = 2001
	target.IsNPC = false
	target.X, target.Z = 1.2, 0
	target.Radius = 0.4

	area.AddActor(npc)
	area.AddActor(target)

	now := time.Now().UnixMilli()
	npc.Mu.Lock()
	npc.SpecialWindupUntil = now - 1
	npc.SpecialTargetRID = target.RuntimeID
	npc.Mu.Unlock()

	target.Mu.Lock()
	startHP := target.Health
	target.LastParryAt = now - 100
	target.ParryUntil = now + 200
	target.Mu.Unlock()

	handled, killed := ProcessNPCSpecialAttack(area, npc, target, now)
	if !handled {
		t.Fatalf("expected special flow to handle active windup")
	}
	if killed {
		t.Fatalf("parried special should not kill target")
	}

	target.Mu.Lock()
	gotHP := target.Health
	target.Mu.Unlock()
	if gotHP != startHP {
		t.Fatalf("parried special should not change HP: got=%d want=%d", gotHP, startHP)
	}

	npc.Mu.Lock()
	if npc.SpecialWindupUntil != 0 || npc.SpecialTargetRID != 0 {
		npc.Mu.Unlock()
		t.Fatalf("expected special windup state to be cleared after resolve")
	}
	npc.Mu.Unlock()
}

func TestProcessNPCSpecialAttackHitsHardWithoutParry(t *testing.T) {
	area := NewArea("Test Area")
	npc := newCombatActor("Mob")
	target := newCombatActor("Player")

	npc.RuntimeID = 1002
	npc.IsNPC = true
	npc.X, npc.Z = 0, 0
	npc.Radius = 0.4
	npc.AttackRange = 2.0
	npc.WeaponDamage = 40
	npc.Strength = 50

	target.RuntimeID = 2002
	target.IsNPC = false
	target.X, target.Z = 1.1, 0
	target.Radius = 0.4
	target.CachedArmor = 5

	area.AddActor(npc)
	area.AddActor(target)

	now := time.Now().UnixMilli()
	npc.Mu.Lock()
	npc.SpecialWindupUntil = now - 1
	npc.SpecialTargetRID = target.RuntimeID
	npc.Mu.Unlock()

	target.Mu.Lock()
	startHP := target.Health
	target.LastParryAt = now - 800
	target.ParryUntil = now - 100
	target.Mu.Unlock()

	handled, killed := ProcessNPCSpecialAttack(area, npc, target, now)
	if !handled {
		t.Fatalf("expected special flow to handle active windup")
	}
	if killed {
		t.Fatalf("target should survive this test scenario")
	}

	target.Mu.Lock()
	gotHP := target.Health
	target.Mu.Unlock()
	if gotHP >= startHP {
		t.Fatalf("special hit should deal damage: start=%d got=%d", startHP, gotHP)
	}
}

func TestProcessNPCSpecialAttackInvokesKillHookOnLethalHit(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
		SetSpecialKillHook(nil)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{
			ID:            9901,
			Name:          "Lethal Test",
			Enabled:       true,
			RangeMax:      3.0,
			ParryWindowMs: 220,
			BaseDamageMin: 500,
			BaseDamageMax: 500,
		},
	})

	area := NewArea("Test Area")
	npc := newCombatActor("Mob")
	target := newCombatActor("Victim")

	npc.RuntimeID = 1003
	npc.IsNPC = false
	npc.X, npc.Z = 0, 0
	npc.Radius = 0.4
	npc.AttackRange = 2.0

	target.RuntimeID = 2003
	target.IsNPC = true
	target.X, target.Z = 1.1, 0
	target.Radius = 0.4
	target.Health = 100
	target.HealthMax = 100

	area.AddActor(npc)
	area.AddActor(target)

	var hookCalled bool
	var hookTargetRID uint32
	SetSpecialKillHook(func(_ *Area, _ *Actor, dead *Actor) {
		hookCalled = true
		if dead != nil {
			hookTargetRID = dead.RuntimeID
		}
	})

	now := time.Now().UnixMilli()
	npc.Mu.Lock()
	npc.SpecialWindupUntil = now - 1
	npc.SpecialTargetRID = target.RuntimeID
	npc.SpecialAbilityID = 9901
	npc.Mu.Unlock()

	handled, killed := ProcessNPCSpecialAttack(area, npc, target, now)
	if !handled {
		t.Fatalf("expected special flow to handle active windup")
	}
	if !killed {
		t.Fatalf("expected lethal special hit")
	}
	if !hookCalled {
		t.Fatalf("expected special kill hook to be invoked")
	}
	if hookTargetRID != target.RuntimeID {
		t.Fatalf("hook target rid mismatch: got=%d want=%d", hookTargetRID, target.RuntimeID)
	}
}
