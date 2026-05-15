package scripting

import (
	"testing"

	"realm-crafter/server/internal/world"
)

func TestDispatchPlayerBeforeCastIntentMergesAdvice(t *testing.T) {
	w := world.New()
	area := w.GetOrCreateArea("Starter Zone")
	reg := New(w)
	defer reg.Close()

	player := world.NewActor()
	player.RuntimeID = 1001
	player.IsNPC = false
	player.AreaName = "Starter Zone"
	area.AddActor(player)

	target := world.NewActor()
	target.RuntimeID = 2002
	target.IsNPC = true
	target.AreaName = "Starter Zone"
	area.AddActor(target)

	if err := reg.L.DoString(`
		Event.on("player_before_cast_intent", function(player_id, target_id, ability_id, reason_tag)
			return {
				action_override = "AttackHeavy",
				reason_tag = "script_combo"
			}
		end)
		Event.on("player_before_cast_intent", function(player_id, target_id, ability_id, reason_tag)
			return {
				client_trace_id = "lua-trace-001"
			}
		end)
	`); err != nil {
		t.Fatalf("register player_before_cast_intent handlers failed: %v", err)
	}

	advice := reg.DispatchPlayerBeforeCastIntent(area, player, target, 7301, "player_input")
	if advice.ActionOverride != "AttackHeavy" {
		t.Fatalf("action override mismatch: got=%q want=%q", advice.ActionOverride, "AttackHeavy")
	}
	if advice.ReasonTag != "script_combo" {
		t.Fatalf("reason tag mismatch: got=%q want=%q", advice.ReasonTag, "script_combo")
	}
	if advice.ClientTraceID != "lua-trace-001" {
		t.Fatalf("trace id mismatch: got=%q want=%q", advice.ClientTraceID, "lua-trace-001")
	}
	if advice.Cancel {
		t.Fatalf("cancel mismatch: got=true want=false")
	}
}

func TestDispatchPlayerBeforeCastIntentCanCancel(t *testing.T) {
	w := world.New()
	area := w.GetOrCreateArea("Starter Zone")
	reg := New(w)
	defer reg.Close()

	player := world.NewActor()
	player.RuntimeID = 1001
	player.IsNPC = false
	player.AreaName = "Starter Zone"
	area.AddActor(player)

	target := world.NewActor()
	target.RuntimeID = 2002
	target.IsNPC = true
	target.AreaName = "Starter Zone"
	area.AddActor(target)

	if err := reg.L.DoString(`
		Event.on("player_before_cast_intent", function(player_id, target_id, ability_id, reason_tag)
			return { cancel = true }
		end)
	`); err != nil {
		t.Fatalf("register cancel handler failed: %v", err)
	}

	advice := reg.DispatchPlayerBeforeCastIntent(area, player, target, 7301, "player_input")
	if !advice.Cancel {
		t.Fatalf("cancel mismatch: got=false want=true")
	}
}
