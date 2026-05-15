package world

import "testing"

func TestSelectNPCSpecialIntentHonorsPhaseTag(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	SetNPCCombatProfiles(nil)
	SetNPCProfileBindings(nil)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
		SetNPCCombatProfiles(nil)
		SetNPCProfileBindings(nil)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{ID: 8101, Name: "P1", Enabled: true, RangeMax: 3.0, CooldownMs: 1000},
		{ID: 8102, Name: "P2", Enabled: true, RangeMax: 3.0, CooldownMs: 1000},
	})
	SetNPCAbilityLoadouts([]NPCAbilityLoadoutEntry{
		{NPCSpawnID: 55, AbilityID: 8101, Priority: 100, Weight: 100, PhaseTag: "phase_1", Enabled: true},
		{NPCSpawnID: 55, AbilityID: 8102, Priority: 100, Weight: 100, PhaseTag: "phase_2", Enabled: true},
	})

	npc := newCombatActor("Mob")
	npc.IsNPC = true
	npc.SpawnID = 55
	npc.AttackRange = 2.0
	npc.X, npc.Y, npc.Z = 0, 0, 0
	npc.Health = 100
	npc.HealthMax = 200 // 50% -> phase_2

	target := newCombatActor("Target")
	target.X, target.Y, target.Z = 1.0, 0, 0

	intent, ok := selectNPCSpecialIntent(npc, target, 5000, 0)
	if !ok {
		t.Fatalf("expected a valid special intent")
	}
	if intent.Ability.ID != 8102 {
		t.Fatalf("phase_tag filter mismatch: got ability=%d want=8102", intent.Ability.ID)
	}
}

func TestSelectNPCSpecialIntentHonorsConditionLua(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	SetNPCCombatProfiles(nil)
	SetNPCProfileBindings(nil)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
		SetNPCCombatProfiles(nil)
		SetNPCProfileBindings(nil)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{ID: 8201, Name: "Execute", Enabled: true, RangeMax: 3.0, CooldownMs: 1000},
		{ID: 8202, Name: "Fallback", Enabled: true, RangeMax: 3.0, CooldownMs: 1000},
	})
	SetNPCAbilityLoadouts([]NPCAbilityLoadoutEntry{
		{
			NPCSpawnID:   56,
			AbilityID:    8201,
			Priority:     200,
			Weight:       100,
			ConditionLua: "target_hp_pct <= 30 and distance <= 2.0",
			Enabled:      true,
		},
		{
			NPCSpawnID: 56,
			AbilityID:  8202,
			Priority:   100,
			Weight:     100,
			Enabled:    true,
		},
	})

	npc := newCombatActor("Mob")
	npc.IsNPC = true
	npc.SpawnID = 56
	npc.AttackRange = 2.0
	npc.X, npc.Y, npc.Z = 0, 0, 0

	target := newCombatActor("Target")
	target.X, target.Y, target.Z = 1.0, 0, 0
	target.Health = 80
	target.HealthMax = 100

	intent, ok := selectNPCSpecialIntent(npc, target, 5000, 0)
	if !ok {
		t.Fatalf("expected fallback special intent when execute condition is false")
	}
	if intent.Ability.ID != 8202 {
		t.Fatalf("expected fallback ability first: got=%d want=8202", intent.Ability.ID)
	}

	target.Health = 20 // 20%
	intent, ok = selectNPCSpecialIntent(npc, target, 6000, 0)
	if !ok {
		t.Fatalf("expected execute intent when condition becomes true")
	}
	if intent.Ability.ID != 8201 {
		t.Fatalf("expected execute ability after condition match: got=%d want=8201", intent.Ability.ID)
	}
}

func TestSelectNPCSpecialIntentInvalidConditionFailsClosed(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	SetNPCCombatProfiles(nil)
	SetNPCProfileBindings(nil)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
		SetNPCCombatProfiles(nil)
		SetNPCProfileBindings(nil)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{ID: 8301, Name: "Broken", Enabled: true, RangeMax: 3.0, CooldownMs: 1000},
		{ID: 8302, Name: "Fallback", Enabled: true, RangeMax: 3.0, CooldownMs: 1000},
	})
	SetNPCAbilityLoadouts([]NPCAbilityLoadoutEntry{
		{
			NPCSpawnID:   57,
			AbilityID:    8301,
			Priority:     200,
			Weight:       100,
			ConditionLua: "target_hp_pct <= and",
			Enabled:      true,
		},
		{
			NPCSpawnID: 57,
			AbilityID:  8302,
			Priority:   100,
			Weight:     100,
			Enabled:    true,
		},
	})

	npc := newCombatActor("Mob")
	npc.IsNPC = true
	npc.SpawnID = 57
	npc.AttackRange = 2.0
	npc.X, npc.Y, npc.Z = 0, 0, 0

	target := newCombatActor("Target")
	target.X, target.Y, target.Z = 1.0, 0, 0

	intent, ok := selectNPCSpecialIntent(npc, target, 5000, 0)
	if !ok {
		t.Fatalf("expected fallback intent even with invalid top condition")
	}
	if intent.Ability.ID != 8302 {
		t.Fatalf("invalid condition should be skipped: got=%d want=8302", intent.Ability.ID)
	}
}
