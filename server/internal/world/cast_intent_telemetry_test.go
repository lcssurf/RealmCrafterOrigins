package world

import "testing"

func TestCastIntentTelemetryAggregatesByReasonTag(t *testing.T) {
	resetCastIntentTelemetry()
	SetAbilityCatalog(nil)
	SetNPCAbilityLoadouts(nil)
	SetAbilityRuntimeEnabled(true)
	defer func() {
		resetCastIntentTelemetry()
		SetAbilityCatalog(nil)
		SetNPCAbilityLoadouts(nil)
		SetAbilityRuntimeEnabled(true)
	}()

	SetAbilityCatalog([]AbilityTemplate{
		{
			ID:         8801,
			Name:       "Telemetry Skill",
			Enabled:    true,
			RangeMax:   3.0,
			CooldownMs: 5000,
			WindupMs:   700,
		},
	})

	w := New()
	area := w.GetOrCreateArea("Telemetry Zone")

	player := NewActor()
	player.RuntimeID = 1001
	player.IsNPC = false
	player.AreaName = "Telemetry Zone"
	player.X, player.Z = 0, 0
	player.Stamina = 100
	player.StaminaMax = 100
	player.Energy = 100
	player.EnergyMax = 100
	area.AddActor(player)

	target := NewActor()
	target.RuntimeID = 2002
	target.IsNPC = true
	target.AreaName = "Telemetry Zone"
	target.X, target.Z = 1.0, 0
	area.AddActor(target)

	const baseNow int64 = 10_000_000

	ok, reason := tryStartCastByRIDAt(w, CastIntent{
		CasterRID: 1001,
		TargetRID: 2002,
		AbilityID: 8801,
		ReasonTag: "boss_phase",
	}, baseNow, castIntentCasterPlayer)
	if !ok || reason != "ok" {
		t.Fatalf("first cast should succeed: ok=%v reason=%s", ok, reason)
	}

	ok, reason = tryStartCastByRIDAt(w, CastIntent{
		CasterRID: 1001,
		TargetRID: 2002,
		AbilityID: 8801,
		ReasonTag: "boss_phase",
	}, baseNow+800, castIntentCasterPlayer)
	if ok {
		t.Fatalf("second cast should fail due cooldown")
	}
	if reason != "global_gcd" && reason != "ability_cooldown" {
		t.Fatalf("unexpected reject reason: %s", reason)
	}

	snap := getCastIntentTelemetrySnapshot()
	if len(snap) != 1 {
		t.Fatalf("telemetry rows mismatch: got=%d want=1", len(snap))
	}
	row := snap[0]
	if row.ReasonTag != "boss_phase" {
		t.Fatalf("reason_tag mismatch: got=%q want=%q", row.ReasonTag, "boss_phase")
	}
	if row.Attempts != 2 || row.Started != 1 || row.Rejected != 1 {
		t.Fatalf("telemetry counters mismatch: attempts=%d started=%d rejected=%d",
			row.Attempts, row.Started, row.Rejected)
	}
	if row.RejectReasons[reason] != 1 {
		t.Fatalf("reject reason counter mismatch: reason=%s count=%d", reason, row.RejectReasons[reason])
	}
}
