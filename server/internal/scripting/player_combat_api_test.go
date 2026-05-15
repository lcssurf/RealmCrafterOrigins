package scripting

import (
	"testing"

	"realm-crafter/server/internal/world"
)

func TestPlayerCombatAPICanCastAndTryCast(t *testing.T) {
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
			ID:                    7301,
			Name:                  "Player API Skill",
			Enabled:               true,
			RangeMax:              3.0,
			CooldownMs:            1000,
			WindupMs:              500,
			ResourceType:          "sp",
			ResourceCost:          15,
			AllowActionOverride:   true,
			AllowedActionTagsJSON: `["heavy"]`,
			ActionWindup:          "Attack",
		},
	})

	w := world.New()
	area := w.GetOrCreateArea("Starter Zone")
	reg := New(w)
	defer reg.Close()

	player := world.NewActor()
	player.RuntimeID = 1001
	player.IsNPC = false
	player.AreaName = "Starter Zone"
	player.X, player.Z = 0, 0
	player.AttackRange = 2.0
	player.Stamina = 40
	player.StaminaMax = 100
	player.Appearance = &world.Appearance{
		Anims: []world.AnimBinding{
			{Action: "Attack"},
			{Action: "AttackHeavy"},
		},
	}
	area.AddActor(player)

	target := world.NewActor()
	target.RuntimeID = 2002
	target.IsNPC = true
	target.AreaName = "Starter Zone"
	target.X, target.Z = 1.1, 0
	area.AddActor(target)

	if err := reg.L.DoString(`
		can_ok = PlayerCombat.can_cast(1001, 7301, 2002)
		cast_ok = PlayerCombat.try_cast(1001, 7301, 2002, { action_override = "AttackHeavy", reason_tag = "player_input" })
	`); err != nil {
		t.Fatalf("PlayerCombat API call failed: %v", err)
	}

	if got := reg.L.GetGlobal("can_ok").String(); got != "true" {
		t.Fatalf("PlayerCombat.can_cast mismatch: got=%s want=true", got)
	}
	if got := reg.L.GetGlobal("cast_ok").String(); got != "true" {
		t.Fatalf("PlayerCombat.try_cast mismatch: got=%s want=true", got)
	}

	player.Mu.Lock()
	defer player.Mu.Unlock()
	if player.Stamina != 25 {
		t.Fatalf("player SP mismatch after cast: got=%d want=25", player.Stamina)
	}
	if player.SpecialAbilityID != 7301 {
		t.Fatalf("special ability mismatch: got=%d want=7301", player.SpecialAbilityID)
	}
	if player.SpecialActionOverride != "AttackHeavy" {
		t.Fatalf("special action override mismatch: got=%q want=%q", player.SpecialActionOverride, "AttackHeavy")
	}
}
