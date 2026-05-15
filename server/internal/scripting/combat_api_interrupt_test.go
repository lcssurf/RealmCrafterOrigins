package scripting

import (
	"testing"
	"time"

	"realm-crafter/server/internal/world"
)

func TestCombatAPIInterruptClearsDefensiveState(t *testing.T) {
	w := world.New()
	area := w.GetOrCreateArea("Starter Zone")
	reg := New(w)
	defer reg.Close()

	caster := world.NewActor()
	caster.RuntimeID = 1001
	caster.Name = "Caster"
	caster.AreaName = "Starter Zone"
	area.AddActor(caster)

	target := world.NewActor()
	target.RuntimeID = 2002
	target.Name = "Target"
	target.AreaName = "Starter Zone"
	target.Guarding = true
	target.GuardUntil = time.Now().UnixMilli() + 1000
	area.AddActor(target)

	reg.ctx = callCtx{
		area:   area,
		caster: caster,
	}
	defer func() { reg.ctx = callCtx{} }()

	if err := reg.L.DoString(`ok = Combat.interrupt(1001, 2002)`); err != nil {
		t.Fatalf("Combat.interrupt failed: %v", err)
	}
	if got := reg.L.GetGlobal("ok").String(); got != "true" {
		t.Fatalf("Combat.interrupt return mismatch: got=%s want=true", got)
	}

	target.Mu.Lock()
	stillGuarding := target.Guarding
	target.Mu.Unlock()
	if stillGuarding {
		t.Fatalf("Combat.interrupt should clear guarding state")
	}

	if err := reg.L.DoString(`ok2 = Combat.interrupt(1001, 2002)`); err != nil {
		t.Fatalf("Combat.interrupt second call failed: %v", err)
	}
	if got := reg.L.GetGlobal("ok2").String(); got != "false" {
		t.Fatalf("Combat.interrupt second call mismatch: got=%s want=false", got)
	}
}
