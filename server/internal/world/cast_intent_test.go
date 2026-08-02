package world

import "testing"

func TestTryStartNPCCastByRIDStartsWindupAndStoresOverride(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{
			ID:                    5001,
			Name:                  "Test Heavy",
			Enabled:               true,
			RangeMax:              3.0,
			WindupMs:              800,
			CooldownMs:            2000,
			AllowActionOverride:   true,
			AllowedActionTagsJSON: `["heavy"]`,
			ActionWindup:          "Attack",
			ActionImpact:          "Attack",
		},
	})

	w := New()
	area := w.GetOrCreateArea("Starter Zone")

	npc := NewActor()
	npc.RuntimeID = 1001
	npc.IsNPC = true
	npc.AreaName = "Starter Zone"
	npc.X, npc.Z = 0, 0
	npc.AttackRange = 2.0
	npc.Stamina = 100
	npc.Energy = 100
	npc.Appearance = &Appearance{
		Anims: []AnimBinding{
			{Action: "Attack"},
			{Action: "AttackHeavy"},
		},
	}
	area.AddActor(npc)

	target := NewActor()
	target.RuntimeID = 2002
	target.AreaName = "Starter Zone"
	target.X, target.Z = 1.0, 0
	area.AddActor(target)

	ok, reason := TryStartNPCCastByRID(w, CastIntent{
		CasterRID:      npc.RuntimeID,
		TargetRID:      target.RuntimeID,
		AbilityID:      5001,
		ActionOverride: "AttackHeavy",
		ReasonTag:      "npc_ai",
		ClientTraceID:  "trace-a",
	})
	if !ok {
		t.Fatalf("TryStartNPCCastByRID should start cast: reason=%s", reason)
	}

	npc.Mu.Lock()
	defer npc.Mu.Unlock()
	if len(npc.PendingImpacts) != 1 {
		t.Fatalf("expected exactly 1 pending impact, got=%d", len(npc.PendingImpacts))
	}
	entry := npc.PendingImpacts[0]
	if entry.Ability.ID != 5001 {
		t.Fatalf("special ability mismatch: got=%d want=5001", entry.Ability.ID)
	}
	if entry.TargetRID != target.RuntimeID {
		t.Fatalf("special target mismatch: got=%d want=%d", entry.TargetRID, target.RuntimeID)
	}
	if entry.ActionOverride != "AttackHeavy" {
		t.Fatalf("special action override mismatch: got=%q want=%q", entry.ActionOverride, "AttackHeavy")
	}
	if entry.ReasonTag != "npc_ai" {
		t.Fatalf("special reason tag mismatch: got=%q", entry.ReasonTag)
	}
	if entry.ClientTraceID != "trace-a" {
		t.Fatalf("special trace mismatch: got=%q", entry.ClientTraceID)
	}
	if entry.ResolveAt <= npc.LastSpecialAt {
		t.Fatalf("windup window not configured: resolveAt=%d lastSpecialAt=%d", entry.ResolveAt, npc.LastSpecialAt)
	}
	if !entry.GatesAbilityCasts {
		t.Fatalf("classic cast path should set GatesAbilityCasts=true")
	}
}

func TestTryStartNPCCastByRIDRejectsInsufficientResource(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{
			ID:           5002,
			Name:         "Stamina Skill",
			Enabled:      true,
			RangeMax:     3.0,
			CooldownMs:   1000,
			ResourceType: "sp",
			ResourceCost: 40,
		},
	})

	w := New()
	area := w.GetOrCreateArea("Starter Zone")

	npc := NewActor()
	npc.RuntimeID = 1001
	npc.IsNPC = true
	npc.AreaName = "Starter Zone"
	npc.AttackRange = 2.0
	npc.Stamina = 15
	area.AddActor(npc)

	target := NewActor()
	target.RuntimeID = 2002
	target.AreaName = "Starter Zone"
	target.X, target.Z = 1.0, 0
	area.AddActor(target)

	ok, reason := TryStartNPCCastByRID(w, CastIntent{
		CasterRID: npc.RuntimeID,
		TargetRID: target.RuntimeID,
		AbilityID: 5002,
	})
	if ok {
		t.Fatalf("TryStartNPCCastByRID should fail for insufficient resource")
	}
	if reason != "resource_insufficient" {
		t.Fatalf("unexpected reject reason: got=%q want=%q", reason, "resource_insufficient")
	}
}

func TestTryStartNPCCastByRIDInvalidOverrideFallsBack(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{
			ID:                    5003,
			Name:                  "Override Test",
			Enabled:               true,
			RangeMax:              3.0,
			CooldownMs:            1000,
			AllowActionOverride:   true,
			AllowedActionTagsJSON: `["heavy"]`,
			ActionWindup:          "Attack",
		},
	})

	w := New()
	area := w.GetOrCreateArea("Starter Zone")

	npc := NewActor()
	npc.RuntimeID = 1001
	npc.IsNPC = true
	npc.AreaName = "Starter Zone"
	npc.AttackRange = 2.0
	npc.Stamina = 100
	npc.Appearance = &Appearance{
		Anims: []AnimBinding{
			{Action: "Attack"},
			{Action: "AttackLight"},
		},
	}
	area.AddActor(npc)

	target := NewActor()
	target.RuntimeID = 2002
	target.AreaName = "Starter Zone"
	target.X, target.Z = 1.0, 0
	area.AddActor(target)

	ok, reason := TryStartNPCCastByRID(w, CastIntent{
		CasterRID:      npc.RuntimeID,
		TargetRID:      target.RuntimeID,
		AbilityID:      5003,
		ActionOverride: "AttackLight",
	})
	if !ok {
		t.Fatalf("TryStartNPCCastByRID should accept and fallback: reason=%s", reason)
	}

	npc.Mu.Lock()
	got := ""
	if len(npc.PendingImpacts) > 0 {
		got = npc.PendingImpacts[len(npc.PendingImpacts)-1].ActionOverride
	}
	npc.Mu.Unlock()
	if got != "" {
		t.Fatalf("invalid override should fallback to empty override, got=%q", got)
	}
}

func TestTryStartPlayerCastByRIDConsumesSPAndStartsWindup(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{
			ID:                    7001,
			Name:                  "Player Dash Strike",
			Enabled:               true,
			RangeMax:              3.0,
			WindupMs:              500,
			CooldownMs:            1000,
			ResourceType:          "sp",
			ResourceCost:          20,
			AllowActionOverride:   true,
			AllowedActionTagsJSON: `["heavy"]`,
			ActionWindup:          "Attack",
		},
	})

	w := New()
	area := w.GetOrCreateArea("Starter Zone")

	player := NewActor()
	player.RuntimeID = 3001
	player.IsNPC = false
	player.AreaName = "Starter Zone"
	player.X, player.Z = 0, 0
	player.AttackRange = 2.0
	player.Stamina = 50
	player.StaminaMax = 100
	player.Appearance = &Appearance{
		Anims: []AnimBinding{
			{Action: "Attack"},
			{Action: "AttackHeavy"},
		},
	}
	area.AddActor(player)

	target := NewActor()
	target.RuntimeID = 4002
	target.IsNPC = true
	target.AreaName = "Starter Zone"
	target.X, target.Z = 1.0, 0
	area.AddActor(target)

	ok, reason := TryStartPlayerCastByRID(w, CastIntent{
		CasterRID:      player.RuntimeID,
		TargetRID:      target.RuntimeID,
		AbilityID:      7001,
		ActionOverride: "AttackHeavy",
		ReasonTag:      "player_input",
	})
	if !ok {
		t.Fatalf("TryStartPlayerCastByRID should start cast: reason=%s", reason)
	}

	player.Mu.Lock()
	defer player.Mu.Unlock()
	if player.Stamina != 30 {
		t.Fatalf("player SP should be consumed: got=%d want=30", player.Stamina)
	}
	if len(player.PendingImpacts) != 1 {
		t.Fatalf("expected exactly 1 pending impact, got=%d", len(player.PendingImpacts))
	}
	entry := player.PendingImpacts[0]
	if entry.Ability.ID != 7001 {
		t.Fatalf("special ability mismatch: got=%d want=7001", entry.Ability.ID)
	}
	if entry.ActionOverride != "AttackHeavy" {
		t.Fatalf("special override mismatch: got=%q want=%q", entry.ActionOverride, "AttackHeavy")
	}
	if entry.ReasonTag != "player_input" {
		t.Fatalf("special reason mismatch: got=%q want=%q", entry.ReasonTag, "player_input")
	}
	if entry.ResolveAt <= player.LastSpecialAt {
		t.Fatalf("windup not configured: resolveAt=%d last=%d", entry.ResolveAt, player.LastSpecialAt)
	}
}

func TestCanPlayerCastByRIDRejectsNPCCaster(t *testing.T) {
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	defer func() {
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{
			ID:       7002,
			Name:     "Any Skill",
			Enabled:  true,
			RangeMax: 3.0,
		},
	})

	w := New()
	area := w.GetOrCreateArea("Starter Zone")

	npc := NewActor()
	npc.RuntimeID = 5001
	npc.IsNPC = true
	npc.AreaName = "Starter Zone"
	npc.X, npc.Z = 0, 0
	npc.AttackRange = 2.0
	area.AddActor(npc)

	target := NewActor()
	target.RuntimeID = 6002
	target.AreaName = "Starter Zone"
	target.X, target.Z = 1.0, 0
	area.AddActor(target)

	ok, reason := CanPlayerCastByRID(w, CastIntent{
		CasterRID: npc.RuntimeID,
		TargetRID: target.RuntimeID,
		AbilityID: 7002,
	})
	if ok {
		t.Fatalf("CanPlayerCastByRID should reject NPC caster")
	}
	if reason != "caster_not_player" {
		t.Fatalf("unexpected reason: got=%q want=%q", reason, "caster_not_player")
	}
}
