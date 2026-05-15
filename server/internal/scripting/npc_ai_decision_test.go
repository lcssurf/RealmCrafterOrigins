package scripting

import (
	"testing"
	"time"

	"realm-crafter/server/internal/world"
)

func TestDispatchNPCAIDecisionCanStartCastViaScript(t *testing.T) {
	world.SetAbilityCatalog(nil)
	world.SetNPCAbilityLoadouts(nil)
	world.SetAbilityRuntimeEnabled(true)
	defer func() {
		world.SetAbilityCatalog(nil)
		world.SetNPCAbilityLoadouts(nil)
		world.SetAbilityRuntimeEnabled(true)
	}()

	world.SetAbilityCatalog([]world.AbilityTemplate{
		{
			ID:                    6201,
			Name:                  "AI Decide Skill",
			Enabled:               true,
			RangeMax:              3.0,
			CooldownMs:            1000,
			WindupMs:              600,
			AllowActionOverride:   true,
			AllowedActionTagsJSON: `["heavy"]`,
			ActionWindup:          "Attack",
		},
	})

	w := world.New()
	area := w.GetOrCreateArea("Starter Zone")
	reg := New(w)
	defer reg.Close()

	npc := world.NewActor()
	npc.RuntimeID = 1001
	npc.IsNPC = true
	npc.AreaName = "Starter Zone"
	npc.X, npc.Z = 0, 0
	npc.AttackRange = 2.0
	npc.Stamina = 100
	npc.Energy = 100
	npc.Appearance = &world.Appearance{
		Anims: []world.AnimBinding{
			{Action: "Attack"},
			{Action: "AttackHeavy"},
		},
	}
	area.AddActor(npc)

	target := world.NewActor()
	target.RuntimeID = 2002
	target.AreaName = "Starter Zone"
	target.X, target.Z = 1.0, 0
	area.AddActor(target)

	if err := reg.L.DoString(`
		Event.on("npc_ai_decide", function(npc_id, target_id, now_ms)
			NPCCombat.try_cast(npc_id, 6201, target_id, {
				action_override = "AttackHeavy",
				reason_tag = "boss_phase"
			})
		end)
	`); err != nil {
		t.Fatalf("register npc_ai_decide handler failed: %v", err)
	}

	handled := reg.DispatchNPCAIDecision(area, npc, target, time.Now().UnixMilli())
	if !handled {
		t.Fatalf("expected npc_ai_decide handler to start cast")
	}

	npc.Mu.Lock()
	defer npc.Mu.Unlock()
	if npc.SpecialAbilityID != 6201 {
		t.Fatalf("special ability mismatch: got=%d want=6201", npc.SpecialAbilityID)
	}
	if npc.SpecialActionOverride != "AttackHeavy" {
		t.Fatalf("special action override mismatch: got=%q want=%q", npc.SpecialActionOverride, "AttackHeavy")
	}
	if npc.SpecialReasonTag != "boss_phase" {
		t.Fatalf("special reason tag mismatch: got=%q want=%q", npc.SpecialReasonTag, "boss_phase")
	}
}

func TestDispatchNPCAIDecisionSupportsNpcDecideAbilityAlias(t *testing.T) {
	world.SetAbilityCatalog(nil)
	world.SetNPCAbilityLoadouts(nil)
	world.SetAbilityRuntimeEnabled(true)
	defer func() {
		world.SetAbilityCatalog(nil)
		world.SetNPCAbilityLoadouts(nil)
		world.SetAbilityRuntimeEnabled(true)
	}()

	world.SetAbilityCatalog([]world.AbilityTemplate{
		{
			ID:                    6202,
			Name:                  "AI Decide Alias Skill",
			Enabled:               true,
			RangeMax:              3.0,
			CooldownMs:            1000,
			WindupMs:              600,
			AllowActionOverride:   true,
			AllowedActionTagsJSON: `["heavy"]`,
			ActionWindup:          "Attack",
		},
	})

	w := world.New()
	area := w.GetOrCreateArea("Starter Zone")
	reg := New(w)
	defer reg.Close()

	npc := world.NewActor()
	npc.RuntimeID = 1001
	npc.IsNPC = true
	npc.AreaName = "Starter Zone"
	npc.X, npc.Z = 0, 0
	npc.Stamina = 100
	npc.Energy = 100
	npc.Appearance = &world.Appearance{
		Anims: []world.AnimBinding{
			{Action: "Attack"},
			{Action: "AttackHeavy"},
		},
	}
	area.AddActor(npc)

	target := world.NewActor()
	target.RuntimeID = 2002
	target.AreaName = "Starter Zone"
	target.X, target.Z = 1.0, 0
	area.AddActor(target)

	if err := reg.L.DoString(`
		Event.on("npc_decide_ability", function(npc_id, target_id, now_ms)
			NPCCombat.try_cast(npc_id, 6202, target_id, {
				action_override = "AttackHeavy",
				reason_tag = "boss_phase_alias"
			})
		end)
	`); err != nil {
		t.Fatalf("register npc_decide_ability handler failed: %v", err)
	}

	handled := reg.DispatchNPCAIDecision(area, npc, target, time.Now().UnixMilli())
	if !handled {
		t.Fatalf("expected npc_decide_ability handler to start cast")
	}

	npc.Mu.Lock()
	defer npc.Mu.Unlock()
	if npc.SpecialAbilityID != 6202 {
		t.Fatalf("special ability mismatch: got=%d want=6202", npc.SpecialAbilityID)
	}
	if npc.SpecialReasonTag != "boss_phase_alias" {
		t.Fatalf("special reason tag mismatch: got=%q want=%q", npc.SpecialReasonTag, "boss_phase_alias")
	}
}
