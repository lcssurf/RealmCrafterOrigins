package scripting

import (
	"log"

	lua "github.com/yuin/gopher-lua"
	"realm-crafter/server/internal/world"
)

// registerAPI wires all Go→Lua modules into the Lua state.
// Called once during Registry creation before any scripts are loaded.
func (r *Registry) registerAPI() {
	r.registerSpellAPI()
	r.registerCombatAPI()
	r.registerActorAPI()
	r.registerEventAPI()
	r.registerDialogAPI()
	r.registerLogAPI()
}

// ---------------------------------------------------------------------------
// Spell API  —  Spell.define(t) → spell,  Spell.register(spell)
// ---------------------------------------------------------------------------

const luaSpellType = "SpellDef"

func (r *Registry) registerSpellAPI() {
	mod := r.L.NewTable()

	// Spell.define({ id, name, spell_type, ep_cost, cooldown_ms, range, icon })
	r.L.SetField(mod, "define", r.L.NewFunction(func(L *lua.LState) int {
		t := L.CheckTable(1)

		def := &SpellDef{
			ID:         uint16(luaIntField(L, t, "id", 0)),
			Name:       luaStrField(t, "name", "Unnamed"),
			SpellType:  luaStrField(t, "spell_type", "damage"),
			EPCost:     int32(luaIntField(L, t, "ep_cost", 10)),
			CooldownMs: int64(luaIntField(L, t, "cooldown_ms", 2000)),
			Range:      float32(luaNumField(t, "range", 25.0)),
			Icon:       uint8(luaIntField(L, t, "icon", 0)),
		}

		// on_cast is set later via spell:on_cast = function() end
		// OR read from the table if provided inline.
		if fn, ok := t.RawGetString("on_cast").(*lua.LFunction); ok {
			def.onCast = fn
		}

		ud := L.NewUserData()
		ud.Value = def
		L.SetMetatable(ud, L.GetTypeMetatable(luaSpellType))
		L.Push(ud)
		return 1
	}))

	// Spell.register(spell)
	r.L.SetField(mod, "register", r.L.NewFunction(func(L *lua.LState) int {
		ud := L.CheckUserData(1)
		def, ok := ud.Value.(*SpellDef)
		if !ok {
			L.ArgError(1, "expected SpellDef")
			return 0
		}
		if err := r.registerSpell(def); err != nil {
			L.RaiseError("Spell.register: %v", err)
		}
		return 0
	}))

	// Metatable so scripts can do  spell.on_cast = function() ... end
	mt := r.L.NewTypeMetatable(luaSpellType)
	r.L.SetField(mt, "__index", r.L.NewFunction(func(L *lua.LState) int {
		ud := L.CheckUserData(1)
		key := L.CheckString(2)
		def := ud.Value.(*SpellDef)
		switch key {
		case "id":
			L.Push(lua.LNumber(def.ID))
		case "name":
			L.Push(lua.LString(def.Name))
		case "on_cast":
			if def.onCast != nil {
				L.Push(def.onCast)
			} else {
				L.Push(lua.LNil)
			}
		default:
			L.Push(lua.LNil)
		}
		return 1
	}))
	r.L.SetField(mt, "__newindex", r.L.NewFunction(func(L *lua.LState) int {
		ud := L.CheckUserData(1)
		key := L.CheckString(2)
		def := ud.Value.(*SpellDef)
		if key == "on_cast" {
			fn, ok := L.Get(3).(*lua.LFunction)
			if ok {
				def.onCast = fn
			}
		}
		return 0
	}))

	r.L.SetGlobal("Spell", mod)
}

// ---------------------------------------------------------------------------
// Combat API  —  Combat.deal_damage, Combat.heal
// ---------------------------------------------------------------------------

func (r *Registry) registerCombatAPI() {
	mod := r.L.NewTable()

	// Combat.deal_damage(caster_id, target_id, amount [, dmg_type])
	// dmg_type: "magic" (default) | "physical"
	// Returns: died (bool)
	r.L.SetField(mod, "deal_damage", r.L.NewFunction(func(L *lua.LState) int {
		casterRID := uint32(L.CheckNumber(1))
		targetRID := uint32(L.CheckNumber(2))
		amount := int32(L.CheckNumber(3))
		_ = L.OptString(4, "magic") // dmg_type reserved for future use

		area := r.ctx.area
		if area == nil {
			L.Push(lua.LFalse)
			return 1
		}

		target, _ := area.GetActor(targetRID)
		if target == nil {
			L.Push(lua.LFalse)
			return 1
		}

		hp, died := world.ApplyDamage(target, amount, casterRID)

		// Floating number — type 1 = magic (blue on client)
		world.BroadcastFloatingNumber(area, target, int16(amount), 1)
		world.BroadcastHPUpdate(area, target, hp)

		if died {
			r.ctx.killedRID = targetRID
			world.BroadcastActorDead(area, targetRID, casterRID)
		}

		L.Push(lua.LBool(died))
		return 1
	}))

	// Combat.heal(target_id, amount)
	// Heals the actor (or caster if target_id == 0).
	// Returns: new_hp (int)
	r.L.SetField(mod, "heal", r.L.NewFunction(func(L *lua.LState) int {
		targetRID := uint32(L.CheckNumber(1))
		amount := int32(L.CheckNumber(2))

		area := r.ctx.area
		if area == nil {
			L.Push(lua.LNumber(0))
			return 1
		}

		// Heal targets self (caster) if id is 0 or caster's id.
		var actor *world.Actor
		if targetRID == 0 || (r.ctx.caster != nil && targetRID == r.ctx.caster.RuntimeID) {
			actor = r.ctx.caster
		} else {
			actor, _ = area.GetActor(targetRID)
		}
		if actor == nil {
			L.Push(lua.LNumber(0))
			return 1
		}

		newHP := world.ApplyHeal(actor, amount)
		// Positive floating number (green) — use negative amount convention: send as negative for heal display
		world.BroadcastFloatingNumber(area, actor, int16(amount), 1)
		world.BroadcastHPUpdate(area, actor, newHP)

		L.Push(lua.LNumber(newHP))
		return 1
	}))

	r.L.SetGlobal("Combat", mod)
}

// ---------------------------------------------------------------------------
// Actor API  —  Actor.get_hp, Actor.get_level, Actor.get_name, Actor.send_msg
// ---------------------------------------------------------------------------

func (r *Registry) registerActorAPI() {
	mod := r.L.NewTable()

	lookup := func(L *lua.LState) (*world.Actor, bool) {
		rid := uint32(L.CheckNumber(1))
		if r.ctx.area == nil {
			return nil, false
		}
		actor, ok := r.ctx.area.GetActor(rid)
		return actor, ok
	}

	r.L.SetField(mod, "get_hp", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			L.Push(lua.LNumber(0))
			return 1
		}
		actor.Mu.Lock()
		hp := actor.Health
		actor.Mu.Unlock()
		L.Push(lua.LNumber(hp))
		return 1
	}))

	r.L.SetField(mod, "get_max_hp", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			L.Push(lua.LNumber(0))
			return 1
		}
		actor.Mu.Lock()
		v := actor.HealthMax
		actor.Mu.Unlock()
		L.Push(lua.LNumber(v))
		return 1
	}))

	r.L.SetField(mod, "get_level", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			L.Push(lua.LNumber(0))
			return 1
		}
		L.Push(lua.LNumber(actor.Level))
		return 1
	}))

	r.L.SetField(mod, "get_name", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			L.Push(lua.LString(""))
			return 1
		}
		L.Push(lua.LString(actor.Name))
		return 1
	}))

	r.L.SetGlobal("Actor", mod)
}

// ---------------------------------------------------------------------------
// Event API  —  Event.on(name, fn)
// ---------------------------------------------------------------------------

func (r *Registry) registerEventAPI() {
	mod := r.L.NewTable()

	// Event.on("event_name", function(args...) end)
	r.L.SetField(mod, "on", r.L.NewFunction(func(L *lua.LState) int {
		name := L.CheckString(1)
		fn := L.CheckFunction(2)
		r.events[name] = append(r.events[name], fn)
		return 0
	}))

	r.L.SetGlobal("Event", mod)
}

// ---------------------------------------------------------------------------
// Dialog API  —  Dialog.send(text [, options_table])
// ---------------------------------------------------------------------------

func (r *Registry) registerDialogAPI() {
	mod := r.L.NewTable()

	// Dialog.send(text [, {opt1, opt2, ...}])
	// Must be called from within an npc_interact or npc_choice handler.
	r.L.SetField(mod, "send", r.L.NewFunction(func(L *lua.LState) int {
		text := L.CheckString(1)
		var options []string
		if tbl := L.OptTable(2, nil); tbl != nil {
			tbl.ForEach(func(_, v lua.LValue) {
				if s, ok := v.(lua.LString); ok {
					options = append(options, string(s))
				}
			})
		}
		r.ctx.pendingDialog = &DialogPending{Text: text, Options: options}
		return 0
	}))

	r.L.SetGlobal("Dialog", mod)
}

// ---------------------------------------------------------------------------
// Log API  —  Log(msg)
// ---------------------------------------------------------------------------

func (r *Registry) registerLogAPI() {
	r.L.SetGlobal("Log", r.L.NewFunction(func(L *lua.LState) int {
		msg := L.CheckString(1)
		log.Printf("scripting: [lua] %s", msg)
		return 0
	}))
}

// ---------------------------------------------------------------------------
// Lua table helpers
// ---------------------------------------------------------------------------

func luaIntField(L *lua.LState, t *lua.LTable, key string, def int) int {
	v := t.RawGetString(key)
	if n, ok := v.(lua.LNumber); ok {
		return int(n)
	}
	return def
}

func luaNumField(t *lua.LTable, key string, def float64) float64 {
	v := t.RawGetString(key)
	if n, ok := v.(lua.LNumber); ok {
		return float64(n)
	}
	return def
}

func luaStrField(t *lua.LTable, key string, def string) string {
	v := t.RawGetString(key)
	if s, ok := v.(lua.LString); ok {
		return string(s)
	}
	return def
}
