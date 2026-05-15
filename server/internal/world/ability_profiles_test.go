package world

import "testing"

func TestResolveNPCCombatProfileBindingPriority(t *testing.T) {
	SetNPCCombatProfiles(nil)
	SetNPCProfileBindings(nil)
	defer func() {
		SetNPCCombatProfiles(nil)
		SetNPCProfileBindings(nil)
	}()

	SetNPCCombatProfiles([]NPCCombatProfile{
		{ID: 1, Name: "default_profile", GlobalGCDMs: 450, DecisionTickMs: 250, Enabled: true},
		{ID: 2, Name: "spawn_profile", GlobalGCDMs: 900, DecisionTickMs: 350, Enabled: true},
		{ID: 3, Name: "actor_profile", GlobalGCDMs: 1200, DecisionTickMs: 500, Enabled: true},
	})
	SetNPCProfileBindings([]NPCProfileBinding{
		{ID: 10, NPCSpawnID: 100, ProfileID: 2, Enabled: true},
		{ID: 11, ActorDefID: 200, ProfileID: 3, Enabled: true},
	})

	gotSpawn := resolveNPCCombatProfile(100, 200)
	if gotSpawn.ID != 2 {
		t.Fatalf("spawn binding should win: got profile id=%d want=2", gotSpawn.ID)
	}

	gotActor := resolveNPCCombatProfile(0, 200)
	if gotActor.ID != 3 {
		t.Fatalf("actor binding should be used when spawn is missing: got profile id=%d want=3", gotActor.ID)
	}

	gotDefault := resolveNPCCombatProfile(999, 999)
	if gotDefault.ID != 1 {
		t.Fatalf("default profile should be used as fallback: got profile id=%d want=1", gotDefault.ID)
	}
}

func TestConsumeNPCAbilityDecisionBudgetRespectsTick(t *testing.T) {
	SetNPCCombatProfiles(nil)
	SetNPCProfileBindings(nil)
	defer func() {
		SetNPCCombatProfiles(nil)
		SetNPCProfileBindings(nil)
	}()

	SetNPCCombatProfiles([]NPCCombatProfile{
		{ID: 1, Name: "default_profile", DecisionTickMs: 300, Enabled: true},
	})
	SetNPCProfileBindings([]NPCProfileBinding{
		{ID: 10, NPCSpawnID: 77, ProfileID: 1, Enabled: true},
	})

	npc := NewActor()
	npc.IsNPC = true
	npc.SpawnID = 77

	if !consumeNPCAbilityDecisionBudget(npc, 1000) {
		t.Fatalf("first decision should be allowed")
	}
	if consumeNPCAbilityDecisionBudget(npc, 1200) {
		t.Fatalf("decision before tick window should be blocked")
	}
	if !consumeNPCAbilityDecisionBudget(npc, 1300) {
		t.Fatalf("decision at/after tick window should be allowed")
	}
}

func TestCanActorStartAbilityNowUsesProfileGlobalGCD(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCCombatProfiles(nil)
	SetNPCProfileBindings(nil)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCCombatProfiles(nil)
		SetNPCProfileBindings(nil)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{ID: 9001, Name: "Profile GCD Skill", Enabled: true, CooldownMs: 1000, RangeMax: 3.0},
	})
	SetNPCCombatProfiles([]NPCCombatProfile{
		{ID: 1, Name: "default_profile", GlobalGCDMs: 1200, DecisionTickMs: 250, Enabled: true},
	})
	SetNPCProfileBindings([]NPCProfileBinding{
		{ID: 10, NPCSpawnID: 45, ProfileID: 1, Enabled: true},
	})

	npc := NewActor()
	npc.IsNPC = true
	npc.SpawnID = 45
	npc.AttackRange = 2.0
	npc.Stamina = 100
	npc.LastSpecialAt = 1000

	target := NewActor()
	target.AttackRange = 2.0
	target.X = 1.0
	target.Z = 0.0

	reason := canActorStartAbilityNow(npc, target, 9001, 2000)
	if reason != "global_gcd" {
		t.Fatalf("should respect profile global gcd: got=%q want=%q", reason, "global_gcd")
	}
}

func TestCanNPCStartSpecialChainRespectsAllowChainFalse(t *testing.T) {
	SetNPCCombatProfiles(nil)
	SetNPCProfileBindings(nil)
	defer func() {
		SetNPCCombatProfiles(nil)
		SetNPCProfileBindings(nil)
	}()

	SetNPCCombatProfiles([]NPCCombatProfile{
		{
			ID:                     1,
			Name:                   "default_profile",
			GlobalGCDMs:            300,
			AllowChainCast:         false,
			MaxConsecutiveSpecials: 5,
			Enabled:                true,
		},
	})
	SetNPCProfileBindings([]NPCProfileBinding{
		{ID: 10, NPCSpawnID: 81, ProfileID: 1, Enabled: true},
	})

	npc := NewActor()
	npc.IsNPC = true
	npc.SpawnID = 81
	npc.LastSpecialAt = 1000
	npc.SpecialChainCount = 1

	if canNPCStartSpecialChain(npc, 1100) {
		t.Fatalf("allow_chain_cast=false should block immediate consecutive special")
	}
	if !canNPCStartSpecialChain(npc, 2301) {
		t.Fatalf("chain should be allowed again after reset window")
	}
	npc.Mu.Lock()
	got := npc.SpecialChainCount
	npc.Mu.Unlock()
	if got != 0 {
		t.Fatalf("stale chain count should reset after timeout: got=%d want=0", got)
	}
}

func TestCanNPCStartSpecialChainRespectsMaxConsecutive(t *testing.T) {
	SetNPCCombatProfiles(nil)
	SetNPCProfileBindings(nil)
	defer func() {
		SetNPCCombatProfiles(nil)
		SetNPCProfileBindings(nil)
	}()

	SetNPCCombatProfiles([]NPCCombatProfile{
		{
			ID:                     1,
			Name:                   "default_profile",
			GlobalGCDMs:            300,
			AllowChainCast:         true,
			MaxConsecutiveSpecials: 2,
			Enabled:                true,
		},
	})
	SetNPCProfileBindings([]NPCProfileBinding{
		{ID: 10, NPCSpawnID: 82, ProfileID: 1, Enabled: true},
	})

	npc := NewActor()
	npc.IsNPC = true
	npc.SpawnID = 82
	npc.LastSpecialAt = 1000

	npc.SpecialChainCount = 1
	if !canNPCStartSpecialChain(npc, 1100) {
		t.Fatalf("chain count below max should be allowed")
	}

	npc.SpecialChainCount = 2
	if canNPCStartSpecialChain(npc, 1100) {
		t.Fatalf("chain count at max should be blocked")
	}
}

func TestBreakNPCSpecialChainResetsCounter(t *testing.T) {
	npc := NewActor()
	npc.IsNPC = true
	npc.SpecialChainCount = 3

	breakNPCSpecialChain(npc)

	npc.Mu.Lock()
	got := npc.SpecialChainCount
	npc.Mu.Unlock()
	if got != 0 {
		t.Fatalf("special chain counter should reset to zero: got=%d", got)
	}
}
