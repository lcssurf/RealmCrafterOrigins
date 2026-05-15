package scripting

import (
	"testing"

	lua "github.com/yuin/gopher-lua"
	"realm-crafter/server/internal/world"
)

func TestNPCCombatAPICanCastAndTryCast(t *testing.T) {
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
			ID:                    6101,
			Name:                  "Lua Test Skill",
			Enabled:               true,
			RangeMax:              3.0,
			CooldownMs:            1200,
			WindupMs:              700,
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
	target.X, target.Z = 1.2, 0
	area.AddActor(target)

	if err := reg.L.DoString(`
		can_ok = NPCCombat.can_cast(1001, 6101, 2002)
		cast_ok = NPCCombat.try_cast(1001, 6101, 2002, { action_override = "AttackHeavy", reason_tag = "npc_ai" })
	`); err != nil {
		t.Fatalf("NPCCombat API call failed: %v", err)
	}

	if got := reg.L.GetGlobal("can_ok").String(); got != "true" {
		t.Fatalf("NPCCombat.can_cast mismatch: got=%s want=true", got)
	}
	if got := reg.L.GetGlobal("cast_ok").String(); got != "true" {
		t.Fatalf("NPCCombat.try_cast mismatch: got=%s want=true", got)
	}

	npc.Mu.Lock()
	defer npc.Mu.Unlock()
	if npc.SpecialAbilityID != 6101 {
		t.Fatalf("special ability mismatch: got=%d want=6101", npc.SpecialAbilityID)
	}
	if npc.SpecialActionOverride != "AttackHeavy" {
		t.Fatalf("special action override mismatch: got=%q want=%q", npc.SpecialActionOverride, "AttackHeavy")
	}
	if npc.SpecialReasonTag != "npc_ai" {
		t.Fatalf("special reason tag mismatch: got=%q want=%q", npc.SpecialReasonTag, "npc_ai")
	}
}

func TestNPCCombatAPIGetLoadoutAndContext(t *testing.T) {
	world.SetAbilityCatalog(nil)
	world.SetNPCAbilityLoadouts(nil)
	world.SetAbilityRuntimeEnabled(true)
	defer func() {
		world.SetAbilityCatalog(nil)
		world.SetNPCAbilityLoadouts(nil)
		world.SetAbilityRuntimeEnabled(true)
	}()

	world.SetNPCAbilityLoadouts([]world.NPCAbilityLoadoutEntry{
		{
			ID:             1,
			NPCSpawnID:     77,
			ActorDefID:     0,
			AbilityID:      9101,
			Priority:       99,
			Weight:         100,
			MinDistance:    0.5,
			MaxDistance:    4.5,
			MinTargetHPPct: 0,
			MaxTargetHPPct: 100,
			PhaseTag:       "phase_2",
			ConditionLua:   "target_hp_pct <= 30",
			Enabled:        true,
		},
		{
			ID:             2,
			NPCSpawnID:     0,
			ActorDefID:     9,
			AbilityID:      9102,
			Priority:       10,
			Weight:         100,
			MinDistance:    0,
			MaxDistance:    8,
			MinTargetHPPct: 0,
			MaxTargetHPPct: 100,
			PhaseTag:       "any",
			ConditionLua:   "",
			Enabled:        true,
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
	npc.SpawnID = 77
	npc.ActorDefID = 9
	npc.X, npc.Z = 0, 0
	npc.Health = 50
	npc.HealthMax = 100
	npc.Stamina = 60
	npc.StaminaMax = 100
	npc.Energy = 70
	npc.EnergyMax = 100
	area.AddActor(npc)

	target := world.NewActor()
	target.RuntimeID = 2002
	target.AreaName = "Starter Zone"
	target.X, target.Z = 3, 0
	target.Health = 25
	target.HealthMax = 100
	target.Stamina = 80
	target.StaminaMax = 100
	target.Energy = 40
	target.EnergyMax = 100
	area.AddActor(target)

	if err := reg.L.DoString(`
		rows = NPCCombat.get_loadout(1001)
		row_count = #rows
		first_ability = rows[1] and rows[1].ability_id or 0
		ctx = NPCCombat.get_context(1001, 2002)
		ctx_phase = ctx.phase or 0
		ctx_phase_tag = ctx.phase_tag or ""
		ctx_distance = ctx.distance or 0
	`); err != nil {
		t.Fatalf("NPCCombat get_loadout/get_context failed: %v", err)
	}

	if got := reg.L.GetGlobal("row_count"); got.String() != "1" {
		t.Fatalf("loadout row count mismatch: got=%s want=1", got.String())
	}
	if got := reg.L.GetGlobal("first_ability"); got.String() != "9101" {
		t.Fatalf("first ability mismatch: got=%s want=9101", got.String())
	}
	if got := reg.L.GetGlobal("ctx_phase"); got.String() != "2" {
		t.Fatalf("ctx phase mismatch: got=%s want=2", got.String())
	}
	if got := reg.L.GetGlobal("ctx_phase_tag"); got.String() != "phase_2" {
		t.Fatalf("ctx phase tag mismatch: got=%s want=phase_2", got.String())
	}
	if got, ok := reg.L.GetGlobal("ctx_distance").(lua.LNumber); !ok || float64(got) < 2.9 || float64(got) > 3.1 {
		t.Fatalf("ctx distance mismatch: got=%v want~3.0", reg.L.GetGlobal("ctx_distance"))
	}
}
