package scripting

import (
	"log"

	lua "github.com/yuin/gopher-lua"
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

// registerAPI wires all Go→Lua modules into the Lua state.
// Called once during Registry creation before any scripts are loaded.
func (r *Registry) registerAPI() {
	r.registerSpellAPI()
	r.registerCombatAPI()
	r.registerNPCCombatAPI()
	r.registerPlayerCombatAPI()
	r.registerActorAPI()
	r.registerEventAPI()
	r.registerDialogAPI()
	r.registerQuestAPI()
	r.registerWorldAPI()
	r.registerInventoryAPI()
	r.registerTimerAPI()
	r.registerLogAPI()
	r.registerTimeAPI()
	r.registerFXAPI()
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
			AoEType:    uint8(luaIntField(L, t, "aoe_type", 0)),
			AoERadius:  float32(luaNumField(t, "aoe_radius", 0.0)),
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
	// AoE: if the spell has aoe_type > 0 and aoe_radius > 0, the engine
	// automatically spreads damage to nearby actors after hitting the primary target.
	// Returns: died (bool) — true if the primary target died.
	r.L.SetField(mod, "deal_damage", r.L.NewFunction(func(L *lua.LState) int {
		casterRID := uint32(L.CheckNumber(1))
		targetRID := uint32(L.CheckNumber(2))
		amount := int32(L.CheckNumber(3))
		_ = L.OptString(4, "magic")

		area := r.ctx.area
		if area == nil {
			L.Push(lua.LFalse)
			return 1
		}

		emitBloodFX := func(casterRID uint32, target *world.Actor, amount int32) {
			if amount <= 0 || target == nil || !target.IsNPC {
				return
			}
			if world.GetBloodMode() != "all" {
				return
			}
			bloodFX := world.GetBloodFX()
			if bloodFX == "" {
				return
			}
			caster := r.ctx.caster
			if caster == nil {
				if c, ok := area.GetActor(casterRID); ok {
					caster = c
				}
			}
			if caster == nil {
				return
			}
			world.BroadcastBloodFX(area, caster, target, bloodFX)
		}

		primaryDied := false

		// Hit primary target (skipped for ground-AoE where target_id == 0).
		if targetRID != 0 {
			target, _ := area.GetActor(targetRID)
			if target != nil {
				hp, died := world.ApplyDamage(target, amount, casterRID)
				world.BroadcastFloatingNumber(area, target, int16(amount), 1)
				world.BroadcastHPUpdate(area, target, hp)
				emitBloodFX(casterRID, target, amount)
				if died {
					r.ctx.killedRID = targetRID
					world.BroadcastActorDead(area, targetRID, casterRID)
					primaryDied = true
				}
			}
		}

		// AoE splash — find actors within radius of primary target or ground point.
		if r.ctx.aoeRadius > 0 {
			var cx, cz float32
			switch r.ctx.aoeType {
			case 1: // around primary target
				if t, _ := area.GetActor(targetRID); t != nil {
					cx, cz = t.X, t.Z
				}
			case 2: // ground-targeted
				cx, cz = r.ctx.groundX, r.ctx.groundZ
			}
			if cx != 0 || cz != 0 || r.ctx.aoeType == 2 {
				for _, splash := range world.ActorsInRadius(area, cx, cz, r.ctx.aoeRadius) {
					if splash.RuntimeID == targetRID || splash.RuntimeID == casterRID {
						continue // already hit or is the caster
					}
					hp, died := world.ApplyDamage(splash, amount, casterRID)
					world.BroadcastFloatingNumber(area, splash, int16(amount), 1)
					world.BroadcastHPUpdate(area, splash, hp)
					emitBloodFX(casterRID, splash, amount)
					if died && r.ctx.killedRID == 0 {
						r.ctx.killedRID = splash.RuntimeID
						world.BroadcastActorDead(area, splash.RuntimeID, casterRID)
					}
				}
			}
		}

		L.Push(lua.LBool(primaryDied))
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

	// Combat.interrupt(caster_id, target_id)
	// Interrupts defensive states on the target (guard/parry/dodge) when present.
	// Returns: interrupted (bool)
	r.L.SetField(mod, "interrupt", r.L.NewFunction(func(L *lua.LState) int {
		casterRID := uint32(L.CheckNumber(1))
		targetRID := uint32(L.CheckNumber(2))
		if casterRID == 0 || targetRID == 0 || casterRID == targetRID {
			L.Push(lua.LFalse)
			return 1
		}

		area := r.ctx.area
		if area == nil {
			L.Push(lua.LFalse)
			return 1
		}

		caster, _ := area.GetActor(casterRID)
		target, _ := area.GetActor(targetRID)
		if caster == nil || target == nil || target.IsDead() {
			L.Push(lua.LFalse)
			return 1
		}

		now := nowMs()
		target.Mu.Lock()
		interrupted := target.Guarding ||
			target.ParryUntil > now || target.DodgeUntil > now
		if interrupted {
			target.Guarding = false
			target.GuardUntil = 0
			target.ParryUntil = 0
			target.DodgeUntil = 0
			target.LastCombatAt = now
		}
		target.Mu.Unlock()

		if interrupted {
			world.BroadcastCombatEvent(
				area,
				protocol.CombatEventInterruptSuccess,
				casterRID,
				targetRID,
				0,
				"",
			)
		}

		L.Push(lua.LBool(interrupted))
		return 1
	}))

	r.L.SetGlobal("Combat", mod)
}

// ---------------------------------------------------------------------------
// NPCCombat API — NPCCombat.can_cast, NPCCombat.try_cast
// ---------------------------------------------------------------------------

func (r *Registry) registerNPCCombatAPI() {
	mod := r.L.NewTable()

	// NPCCombat.can_cast(npc_id, ability_id, target_id) -> bool
	r.L.SetField(mod, "can_cast", r.L.NewFunction(func(L *lua.LState) int {
		npcRID := uint32(L.CheckNumber(1))
		abilityID := int(L.CheckNumber(2))
		targetRID := uint32(L.CheckNumber(3))
		if r.w == nil || npcRID == 0 || targetRID == 0 || abilityID <= 0 {
			L.Push(lua.LFalse)
			return 1
		}
		ok, _ := world.CanNPCCastByRID(r.w, world.CastIntent{
			CasterRID: npcRID,
			TargetRID: targetRID,
			AbilityID: abilityID,
		})
		L.Push(lua.LBool(ok))
		return 1
	}))

	// NPCCombat.try_cast(npc_id, ability_id, target_id [, opts]) -> bool
	// opts = { action_override="CastHeavy", reason_tag="npc_ai", client_trace_id="..." }
	r.L.SetField(mod, "try_cast", r.L.NewFunction(func(L *lua.LState) int {
		npcRID := uint32(L.CheckNumber(1))
		abilityID := int(L.CheckNumber(2))
		targetRID := uint32(L.CheckNumber(3))
		if r.w == nil || npcRID == 0 || targetRID == 0 || abilityID <= 0 {
			L.Push(lua.LFalse)
			return 1
		}
		intent := world.CastIntent{
			CasterRID: npcRID,
			TargetRID: targetRID,
			AbilityID: abilityID,
			ReasonTag: "npc_ai",
		}
		if opts := L.OptTable(4, nil); opts != nil {
			intent.ActionOverride = luaStrField(opts, "action_override", "")
			if tag := luaStrField(opts, "reason_tag", ""); tag != "" {
				intent.ReasonTag = tag
			}
			intent.ClientTraceID = luaStrField(opts, "client_trace_id", "")
		}

		ok, reason := world.TryStartNPCCastByRID(r.w, intent)
		if !ok {
			log.Printf("scripting: NPCCombat.try_cast rejected npc=%d ability=%d target=%d reason=%s",
				npcRID, abilityID, targetRID, reason)
		}
		L.Push(lua.LBool(ok))
		return 1
	}))

	// NPCCombat.get_loadout(npc_id) -> table[]
	// Returns effective loadout rows already resolved by spawn/actor_def priority.
	r.L.SetField(mod, "get_loadout", r.L.NewFunction(func(L *lua.LState) int {
		npcRID := uint32(L.CheckNumber(1))
		if r.w == nil || npcRID == 0 {
			L.Push(r.L.NewTable())
			return 1
		}
		rows := world.GetNPCAbilityLoadoutByRID(r.w, npcRID)
		out := r.L.NewTable()
		for _, row := range rows {
			item := r.L.NewTable()
			item.RawSetString("id", lua.LNumber(row.ID))
			item.RawSetString("npc_spawn_id", lua.LNumber(row.NPCSpawnID))
			item.RawSetString("actor_def_id", lua.LNumber(row.ActorDefID))
			item.RawSetString("ability_id", lua.LNumber(row.AbilityID))
			item.RawSetString("priority", lua.LNumber(row.Priority))
			item.RawSetString("weight", lua.LNumber(row.Weight))
			item.RawSetString("min_distance", lua.LNumber(row.MinDistance))
			item.RawSetString("max_distance", lua.LNumber(row.MaxDistance))
			item.RawSetString("min_target_hp_pct", lua.LNumber(row.MinTargetHPPct))
			item.RawSetString("max_target_hp_pct", lua.LNumber(row.MaxTargetHPPct))
			item.RawSetString("phase_tag", lua.LString(row.PhaseTag))
			item.RawSetString("condition_lua", lua.LString(row.ConditionLua))
			item.RawSetString("enabled", lua.LBool(row.Enabled))
			out.Append(item)
		}
		L.Push(out)
		return 1
	}))

	// NPCCombat.get_context(npc_id, target_id) -> table
	// Returns runtime context snapshot used by condition checks.
	r.L.SetField(mod, "get_context", r.L.NewFunction(func(L *lua.LState) int {
		npcRID := uint32(L.CheckNumber(1))
		targetRID := uint32(L.CheckNumber(2))
		out := r.L.NewTable()
		if r.w == nil || npcRID == 0 || targetRID == 0 {
			L.Push(out)
			return 1
		}
		ctx, ok := world.GetNPCAbilityDecisionContextByRID(r.w, npcRID, targetRID)
		if !ok {
			L.Push(out)
			return 1
		}
		out.RawSetString("distance", lua.LNumber(ctx.Distance))
		out.RawSetString("npc_hp_pct", lua.LNumber(ctx.NPCHPPct))
		out.RawSetString("target_hp_pct", lua.LNumber(ctx.TargetHPPct))
		out.RawSetString("npc_sp_pct", lua.LNumber(ctx.NPCSPPct))
		out.RawSetString("target_sp_pct", lua.LNumber(ctx.TargetSPPct))
		out.RawSetString("npc_mp_pct", lua.LNumber(ctx.NPCMPPct))
		out.RawSetString("target_mp_pct", lua.LNumber(ctx.TargetMPPct))
		out.RawSetString("phase_tag", lua.LString(ctx.PhaseTag))
		out.RawSetString("phase", lua.LNumber(ctx.Phase))
		L.Push(out)
		return 1
	}))

	// NPCCombat.spawn_impact(npc_id, x, z, radius, damage_min, damage_max, delay_ms [, opts]) -> impact_id | false
	// opts = { telegraph_color="1,0.3,0.05,0.8", telegraph_style="ring_close",
	//          vfx_path_impact="vfx:explosion", action_override="", reason_tag="" }
	//
	// The generic "schedule ONE ground impact" primitive — telegraph + AoE
	// damage, reusing the exact same resolution pipeline as a normal special
	// attack (ActorsInRadius/IsHostileTo/ground telegraph), but with no
	// personal target and no windup-exclusivity (stacking several calls
	// never blocks the caster's normal ability rotation). A "meteor shower"
	// is just a script loop calling this N times with its OWN chosen
	// positions/timing (random, staggered, in a line, whatever the specific
	// encounter needs) — there is no fixed pattern baked into the engine.
	// See docs/TECH_DEBT.md, mob AoE + multi-impact investigation.
	r.L.SetField(mod, "spawn_impact", r.L.NewFunction(func(L *lua.LState) int {
		npcRID := uint32(L.CheckNumber(1))
		x := float32(L.CheckNumber(2))
		z := float32(L.CheckNumber(3))
		radius := float32(L.CheckNumber(4))
		damageMin := int32(L.CheckNumber(5))
		damageMax := int32(L.CheckNumber(6))
		delayMs := int64(L.CheckNumber(7))
		if r.w == nil || npcRID == 0 {
			L.Push(lua.LFalse)
			return 1
		}

		var telegraphColor, telegraphStyle, vfxPathImpact, actionOverride, reasonTag string
		if opts := L.OptTable(8, nil); opts != nil {
			telegraphColor = luaStrField(opts, "telegraph_color", "")
			telegraphStyle = luaStrField(opts, "telegraph_style", "")
			vfxPathImpact = luaStrField(opts, "vfx_path_impact", "")
			actionOverride = luaStrField(opts, "action_override", "")
			reasonTag = luaStrField(opts, "reason_tag", "")
		}

		id, ok := world.SpawnScriptedImpact(
			r.w, npcRID, x, z, radius, damageMin, damageMax, delayMs,
			telegraphColor, telegraphStyle, vfxPathImpact,
			actionOverride, reasonTag,
			nowMs(),
		)
		if !ok {
			L.Push(lua.LFalse)
			return 1
		}
		L.Push(lua.LNumber(id))
		return 1
	}))

	r.L.SetGlobal("NPCCombat", mod)
}

// ---------------------------------------------------------------------------
// PlayerCombat API — PlayerCombat.can_cast, PlayerCombat.try_cast
// ---------------------------------------------------------------------------

func (r *Registry) registerPlayerCombatAPI() {
	mod := r.L.NewTable()

	// PlayerCombat.can_cast(player_id, ability_id, target_id) -> bool
	r.L.SetField(mod, "can_cast", r.L.NewFunction(func(L *lua.LState) int {
		playerRID := uint32(L.CheckNumber(1))
		abilityID := int(L.CheckNumber(2))
		targetRID := uint32(L.CheckNumber(3))
		if r.w == nil || playerRID == 0 || targetRID == 0 || abilityID <= 0 {
			L.Push(lua.LFalse)
			return 1
		}
		ok, _ := world.CanPlayerCastByRID(r.w, world.CastIntent{
			CasterRID: playerRID,
			TargetRID: targetRID,
			AbilityID: abilityID,
		})
		L.Push(lua.LBool(ok))
		return 1
	}))

	// PlayerCombat.try_cast(player_id, ability_id, target_id [, opts]) -> bool
	// opts = { action_override="AttackHeavy", reason_tag="player_input", client_trace_id="..." }
	r.L.SetField(mod, "try_cast", r.L.NewFunction(func(L *lua.LState) int {
		playerRID := uint32(L.CheckNumber(1))
		abilityID := int(L.CheckNumber(2))
		targetRID := uint32(L.CheckNumber(3))
		if r.w == nil || playerRID == 0 || targetRID == 0 || abilityID <= 0 {
			L.Push(lua.LFalse)
			return 1
		}
		intent := world.CastIntent{
			CasterRID: playerRID,
			TargetRID: targetRID,
			AbilityID: abilityID,
			ReasonTag: "player_input",
		}
		if opts := L.OptTable(4, nil); opts != nil {
			intent.ActionOverride = luaStrField(opts, "action_override", "")
			if tag := luaStrField(opts, "reason_tag", ""); tag != "" {
				intent.ReasonTag = tag
			}
			intent.ClientTraceID = luaStrField(opts, "client_trace_id", "")
		}

		ok, reason := world.TryStartPlayerCastByRID(r.w, intent)
		if !ok {
			log.Printf("scripting: PlayerCombat.try_cast rejected player=%d ability=%d target=%d reason=%s",
				playerRID, abilityID, targetRID, reason)
		}
		L.Push(lua.LBool(ok))
		return 1
	}))

	r.L.SetGlobal("PlayerCombat", mod)
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

	// Actor.get_pos(rid) -> x, y, z — current world position. Needed by any
	// script that computes its own impact points for NPCCombat.spawn_impact
	// (e.g. scattering several around a target for a "meteor shower"
	// pattern) — without this there was no way for a script to know WHERE
	// to spawn a raw impact relative to a target.
	r.L.SetField(mod, "get_pos", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			L.Push(lua.LNumber(0))
			L.Push(lua.LNumber(0))
			L.Push(lua.LNumber(0))
			return 3
		}
		actor.Mu.Lock()
		x, y, z := actor.X, actor.Y, actor.Z
		actor.Mu.Unlock()
		L.Push(lua.LNumber(x))
		L.Push(lua.LNumber(y))
		L.Push(lua.LNumber(z))
		return 3
	}))

	// Actor.play_action(rid, action_name) — semantic action: looks up action_id in
	// the actor's appearance table and broadcasts PAnimateActor to all clients.
	r.L.SetField(mod, "play_action", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			return 0
		}
		actionName := L.CheckString(2)
		if r.ctx.area != nil {
			world.BroadcastAnimate(r.ctx.area, actor, actionName)
		}
		return 0
	}))

	// Actor.play_anim(rid, action_name) — backward-compatible alias for play_action.
	r.L.SetField(mod, "play_anim", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			return 0
		}
		actionName := L.CheckString(2)
		if r.ctx.area != nil {
			world.BroadcastAnimate(r.ctx.area, actor, actionName)
		}
		return 0
	}))

	// Actor.has_action(rid, action_name) — returns bool: true if the actor's
	// appearance table contains a binding with that action name.
	r.L.SetField(mod, "has_action", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			L.Push(lua.LFalse)
			return 1
		}
		actionName := L.CheckString(2)
		found := false
		if actor.Appearance != nil {
			for _, anim := range actor.Appearance.Anims {
				if anim.Action == actionName {
					found = true
					break
				}
			}
		}
		if found {
			L.Push(lua.LTrue)
		} else {
			L.Push(lua.LFalse)
		}
		return 1
	}))

	// Actor.current_action(rid) — returns the name of the last action broadcast
	// for this actor (written by BroadcastAnimate under actor.Mu).
	r.L.SetField(mod, "current_action", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			L.Push(lua.LString(""))
			return 1
		}
		actor.Mu.Lock()
		cur := actor.CurrentAction
		actor.Mu.Unlock()
		L.Push(lua.LString(cur))
		return 1
	}))

	// Actor.send_msg(rid, text) — delivers a chat-log line to just this
	// actor's client (sender shown as "System"). See
	// world.Actor.SendSystemMessage.
	r.L.SetField(mod, "send_msg", r.L.NewFunction(func(L *lua.LState) int {
		actor, ok := lookup(L)
		if !ok {
			return 0
		}
		actor.SendSystemMessage(L.CheckString(2))
		return 0
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

	// Dialog.open_shop() — signals the server to open the NPC's shop for the player.
	r.L.SetField(mod, "open_shop", r.L.NewFunction(func(L *lua.LState) int {
		if r.ctx.pendingDialog == nil {
			r.ctx.pendingDialog = &DialogPending{}
		}
		r.ctx.pendingDialog.OpenShop = true
		return 0
	}))

	r.L.SetGlobal("Dialog", mod)
}

// ---------------------------------------------------------------------------
// Log API  —  Log(msg)
// ---------------------------------------------------------------------------

func (r *Registry) registerQuestAPI() {
	mod := r.L.NewTable()

	questCall := func(L *lua.LState, fn func(QuestBridge, uint32, int) (bool, error)) int {
		playerRID := uint32(L.CheckNumber(1))
		questID := int(L.CheckNumber(2))
		if playerRID == 0 || questID <= 0 || r.quest == nil {
			L.Push(lua.LFalse)
			return 1
		}
		changed, err := fn(r.quest, playerRID, questID)
		if err != nil {
			log.Printf("scripting: quest op failed player=%d quest=%d err=%v", playerRID, questID, err)
			L.Push(lua.LFalse)
			return 1
		}
		L.Push(lua.LBool(changed))
		return 1
	}

	// Quest.accept(player_id, quest_id) -> changed(bool)
	r.L.SetField(mod, "accept", r.L.NewFunction(func(L *lua.LState) int {
		return questCall(L, func(q QuestBridge, playerRID uint32, questID int) (bool, error) {
			return q.Accept(playerRID, questID)
		})
	}))

	// Quest.abandon(player_id, quest_id) -> changed(bool)
	r.L.SetField(mod, "abandon", r.L.NewFunction(func(L *lua.LState) int {
		return questCall(L, func(q QuestBridge, playerRID uint32, questID int) (bool, error) {
			return q.Abandon(playerRID, questID)
		})
	}))

	// Quest.turn_in(player_id, quest_id) -> changed(bool)
	r.L.SetField(mod, "turn_in", r.L.NewFunction(func(L *lua.LState) int {
		return questCall(L, func(q QuestBridge, playerRID uint32, questID int) (bool, error) {
			return q.TurnIn(playerRID, questID)
		})
	}))

	// Quest.sync(player_id) -> ok(bool)
	r.L.SetField(mod, "sync", r.L.NewFunction(func(L *lua.LState) int {
		playerRID := uint32(L.CheckNumber(1))
		if playerRID == 0 || r.quest == nil {
			L.Push(lua.LFalse)
			return 1
		}
		if err := r.quest.Sync(playerRID); err != nil {
			log.Printf("scripting: quest sync failed player=%d err=%v", playerRID, err)
			L.Push(lua.LFalse)
			return 1
		}
		L.Push(lua.LTrue)
		return 1
	}))

	progressCall := func(L *lua.LState, event QuestProgressEvent) int {
		playerRID := uint32(L.CheckNumber(1))
		if playerRID == 0 || r.quest == nil {
			L.Push(lua.LFalse)
			return 1
		}
		changed, err := r.quest.Progress(playerRID, event)
		if err != nil {
			log.Printf("scripting: quest progress failed player=%d type=%d err=%v",
				playerRID, event.ObjectiveType, err)
			L.Push(lua.LFalse)
			return 1
		}
		L.Push(lua.LBool(changed))
		return 1
	}

	// Quest.progress(player_id, objective_type, {npc_name="", item_id=0, area="", delta=1}) -> changed(bool)
	r.L.SetField(mod, "progress", r.L.NewFunction(func(L *lua.LState) int {
		objectiveType := uint8(L.CheckNumber(2))
		event := QuestProgressEvent{
			ObjectiveType: objectiveType,
			Delta:         1,
		}
		if t := L.OptTable(3, nil); t != nil {
			event.TargetNPCName = luaStrField(t, "npc_name", "")
			event.TargetItemID = uint16(luaIntField(L, t, "item_id", 0))
			event.TargetArea = luaStrField(t, "area", "")
			event.Delta = luaIntField(L, t, "delta", 1)
		}
		return progressCall(L, event)
	}))

	// Quest.progress_kill(player_id, npc_name [, delta=1]) -> changed(bool)
	r.L.SetField(mod, "progress_kill", r.L.NewFunction(func(L *lua.LState) int {
		event := QuestProgressEvent{
			ObjectiveType: QuestObjectiveKill,
			TargetNPCName: L.CheckString(2),
			Delta:         L.OptInt(3, 1),
		}
		return progressCall(L, event)
	}))

	// Quest.progress_collect(player_id, item_id [, delta=1]) -> changed(bool)
	r.L.SetField(mod, "progress_collect", r.L.NewFunction(func(L *lua.LState) int {
		event := QuestProgressEvent{
			ObjectiveType: QuestObjectiveCollect,
			TargetItemID:  uint16(L.CheckInt(2)),
			Delta:         L.OptInt(3, 1),
		}
		return progressCall(L, event)
	}))

	// Quest.progress_talk(player_id, npc_name [, delta=1]) -> changed(bool)
	r.L.SetField(mod, "progress_talk", r.L.NewFunction(func(L *lua.LState) int {
		event := QuestProgressEvent{
			ObjectiveType: QuestObjectiveTalk,
			TargetNPCName: L.CheckString(2),
			Delta:         L.OptInt(3, 1),
		}
		return progressCall(L, event)
	}))

	// Quest.progress_explore(player_id, area_name [, delta=1]) -> changed(bool)
	r.L.SetField(mod, "progress_explore", r.L.NewFunction(func(L *lua.LState) int {
		event := QuestProgressEvent{
			ObjectiveType: QuestObjectiveExplore,
			TargetArea:    L.CheckString(2),
			Delta:         L.OptInt(3, 1),
		}
		return progressCall(L, event)
	}))

	// Quest.progress_interact(player_id, npc_name [, delta=1]) -> changed(bool)
	r.L.SetField(mod, "progress_interact", r.L.NewFunction(func(L *lua.LState) int {
		event := QuestProgressEvent{
			ObjectiveType: QuestObjectiveInteract,
			TargetNPCName: L.CheckString(2),
			Delta:         L.OptInt(3, 1),
		}
		return progressCall(L, event)
	}))

	// Objective type constants exposed to Lua scripts.
	r.L.SetField(mod, "TYPE_KILL", lua.LNumber(QuestObjectiveKill))
	r.L.SetField(mod, "TYPE_COLLECT", lua.LNumber(QuestObjectiveCollect))
	r.L.SetField(mod, "TYPE_TALK", lua.LNumber(QuestObjectiveTalk))
	r.L.SetField(mod, "TYPE_EXPLORE", lua.LNumber(QuestObjectiveExplore))
	r.L.SetField(mod, "TYPE_INTERACT", lua.LNumber(QuestObjectiveInteract))

	r.L.SetGlobal("Quest", mod)
}

// ---------------------------------------------------------------------------
// World API  —  World.SetTransform/SetVisible/SetCollision
//
// Runtime control of placed WorldObjects (see world.WorldObject/
// WorldObjectState/Area.objectOverrides). Every call resolves the current
// area from r.ctx.area (populated for the duration of the Lua call — see
// callCtx — by the same call sites that already set it for
// npc_interact/npc_choice/etc.), so these must be called from within a
// script handler fired while an area context is active. Each call updates
// Area.objectOverrides (storing the FINAL settled state, never a mid-flight
// one) and broadcasts PWorldObjectUpdate to every client in the area.
// ---------------------------------------------------------------------------

func (r *Registry) registerWorldAPI() {
	mod := r.L.NewTable()

	// World.SetTransform(object_id, {x=,y=,z=}, yaw, duration) -> ok(bool)
	// Animates the object from its current position/yaw to the given target
	// over duration seconds (0 or omitted = instant). Scale is left
	// unchanged — this call does not expose it.
	r.L.SetField(mod, "SetTransform", r.L.NewFunction(func(L *lua.LState) int {
		area := r.ctx.area
		if area == nil {
			L.Push(lua.LFalse)
			return 1
		}
		objectID := int(L.CheckNumber(1))
		posTbl := L.CheckTable(2)
		yaw := float32(L.CheckNumber(3))
		duration := float32(L.OptNumber(4, 0))

		cur, ok := area.CurrentWorldObjectState(objectID)
		if !ok {
			L.Push(lua.LFalse)
			return 1
		}
		x := float32(luaNumField(posTbl, "x", float64(cur.X)))
		y := float32(luaNumField(posTbl, "y", float64(cur.Y)))
		z := float32(luaNumField(posTbl, "z", float64(cur.Z)))

		L.Push(lua.LBool(area.UpdateWorldObjectTransform(objectID, x, y, z, yaw, cur.Scale, duration)))
		return 1
	}))

	// World.SetVisible(object_id, visible) -> ok(bool)
	// Applies immediately (never interpolated).
	r.L.SetField(mod, "SetVisible", r.L.NewFunction(func(L *lua.LState) int {
		area := r.ctx.area
		if area == nil {
			L.Push(lua.LFalse)
			return 1
		}
		objectID := int(L.CheckNumber(1))
		visible := L.CheckBool(2)
		L.Push(lua.LBool(area.SetWorldObjectVisible(objectID, visible)))
		return 1
	}))

	// World.SetCollision(object_id, collision) -> ok(bool)
	// Applies immediately (never interpolated).
	r.L.SetField(mod, "SetCollision", r.L.NewFunction(func(L *lua.LState) int {
		area := r.ctx.area
		if area == nil {
			L.Push(lua.LFalse)
			return 1
		}
		objectID := int(L.CheckNumber(1))
		collision := L.CheckBool(2)
		L.Push(lua.LBool(area.SetWorldObjectCollision(objectID, collision)))
		return 1
	}))

	// World.GetTransform(object_id) -> {x,y,z,yaw,pitch,roll,scale} or nil
	// Returns the object's CURRENT resolved transform — an active override
	// if SetTransform has ever moved it, otherwise the static authored pose
	// from zone_scenery (same source SetTransform's own "cur" comes from,
	// CurrentWorldObjectState). Lets a script capture an object's REAL pose
	// (e.g. a door's true "closed" position) instead of hand-typing world
	// coordinates that silently drift out of sync whenever the object is
	// repositioned in the GUE (see door_key.lua, which does exactly this on
	// first interact instead of hardcoding CLOSED_Y/DOOR_YAW).
	r.L.SetField(mod, "GetTransform", r.L.NewFunction(func(L *lua.LState) int {
		area := r.ctx.area
		if area == nil {
			L.Push(lua.LNil)
			return 1
		}
		objectID := int(L.CheckNumber(1))
		cur, ok := area.CurrentWorldObjectState(objectID)
		if !ok {
			L.Push(lua.LNil)
			return 1
		}
		t := L.NewTable()
		t.RawSetString("x", lua.LNumber(cur.X))
		t.RawSetString("y", lua.LNumber(cur.Y))
		t.RawSetString("z", lua.LNumber(cur.Z))
		t.RawSetString("yaw", lua.LNumber(cur.Yaw))
		t.RawSetString("pitch", lua.LNumber(cur.Pitch))
		t.RawSetString("roll", lua.LNumber(cur.Roll))
		t.RawSetString("scale", lua.LNumber(cur.Scale))
		L.Push(t)
		return 1
	}))

	// World.SpawnTempProp(model_path, start_pos, end_pos, duration_ms [, scale] [, on_arrival_fx_key]) -> prop_id
	// start_pos/end_pos = {x=,y=,z=[,yaw=]}. Creates an EPHEMERAL WorldObject
	// (not zone_scenery-backed, never persisted — see Area.SpawnEphemeralObject)
	// that the client animates from start to end over duration_ms using the
	// SAME ease-in-out transform interpolation PWorldObjectUpdate already
	// drives for authored scenery, then auto-removes. Independent of
	// NPCCombat.spawn_impact (gameplay/damage) — a script synchronizes the
	// two purely by passing the same delay/duration to both, e.g. a falling
	// meteor mesh (this) landing at the same moment an AoE impact (that)
	// resolves. See docs/TECH_DEBT.md, FX.play + SpawnTempProp investigation.
	//
	// scale (optional, default 1.0): raw model files are typically authored
	// at a "natural" size meant to be scaled DOWN per placement — the same
	// way zone_scenery.sx works for authored scenery (LoadWorldObjects,
	// db.go). This was previously hardcoded to 1.0 with no way to override
	// it, which is why a first test (a small decorative rock model) came
	// out gigantic — nothing about SpawnTempProp itself was broken, the
	// scale was just never exposed to the caller. Fixed here, not by
	// picking a different default model.
	r.L.SetField(mod, "SpawnTempProp", r.L.NewFunction(func(L *lua.LState) int {
		area := r.ctx.area
		if area == nil {
			L.Push(lua.LNumber(0))
			return 1
		}
		modelPath := L.CheckString(1)
		startTbl := L.CheckTable(2)
		endTbl := L.CheckTable(3)
		durationMs := int64(L.CheckNumber(4))
		scale := float32(L.OptNumber(5, 1.0))
		onArrivalFXKey := L.OptString(6, "")

		startX := float32(luaNumField(startTbl, "x", 0))
		startY := float32(luaNumField(startTbl, "y", 0))
		startZ := float32(luaNumField(startTbl, "z", 0))
		startYaw := float32(luaNumField(startTbl, "yaw", 0))
		endX := float32(luaNumField(endTbl, "x", 0))
		endY := float32(luaNumField(endTbl, "y", 0))
		endZ := float32(luaNumField(endTbl, "z", 0))
		endYaw := float32(luaNumField(endTbl, "yaw", 0))

		id := area.SpawnEphemeralObject(
			modelPath, scale,
			startX, startY, startZ, startYaw,
			endX, endY, endZ, endYaw,
			durationMs, onArrivalFXKey,
		)
		L.Push(lua.LNumber(id))
		return 1
	}))

	// World.TeleportToAreaSpawn(player_id) -> ok(bool)
	// Resets the player's live position to their cached spawn coordinates
	// (Actor.SpawnX/Y/Z/Yaw, resolved once at login by handleStartGame — see
	// net/client.go) and snaps the client via PRepositionActor. Reuses
	// world.Actor.TeleportToSpawn() — the exact same reset already used by
	// handleRespawnPlayer on death — so spawn resolution is never
	// re-derived here. Returns false if the actor id doesn't resolve in
	// this area, or if it has no valid cached spawn yet.
	r.L.SetField(mod, "TeleportToAreaSpawn", r.L.NewFunction(func(L *lua.LState) int {
		area := r.ctx.area
		if area == nil {
			L.Push(lua.LFalse)
			return 1
		}
		rid := uint32(L.CheckNumber(1))
		actor, ok := area.GetActor(rid)
		if !ok {
			L.Push(lua.LFalse)
			return 1
		}
		// area.Heightmap lets TeleportToSpawn snap Y to the real terrain at
		// (SpawnX, SpawnZ) instead of trusting the authored SpawnY, which can
		// silently drift out of sync with the terrain (see TeleportToSpawn's
		// doc-comment) and freeze the player's movement entirely if it does.
		L.Push(lua.LBool(actor.TeleportToSpawn(area.Heightmap)))
		return 1
	}))

	r.L.SetGlobal("World", mod)
}

// ---------------------------------------------------------------------------
// Inventory API  —  Inventory.has_item/Inventory.remove_item
//
// Delegates to r.inventory (InventoryBridge, injected via SetInventoryBridge
// — see registry.go), mirroring the Quest.* API's use of r.quest (QuestBridge).
// ---------------------------------------------------------------------------

func (r *Registry) registerInventoryAPI() {
	mod := r.L.NewTable()

	// Inventory.has_item(player_id, item_id [, qty=1]) -> bool
	r.L.SetField(mod, "has_item", r.L.NewFunction(func(L *lua.LState) int {
		if r.inventory == nil {
			L.Push(lua.LFalse)
			return 1
		}
		playerRID := uint32(L.CheckNumber(1))
		itemID := uint16(L.CheckNumber(2))
		qty := L.OptInt(3, 1)
		has, err := r.inventory.HasItem(playerRID, itemID, qty)
		if err != nil {
			log.Printf("scripting: inventory has_item failed player=%d item=%d err=%v", playerRID, itemID, err)
			L.Push(lua.LFalse)
			return 1
		}
		L.Push(lua.LBool(has))
		return 1
	}))

	// Inventory.remove_item(player_id, item_id [, qty=1]) -> ok(bool)
	r.L.SetField(mod, "remove_item", r.L.NewFunction(func(L *lua.LState) int {
		if r.inventory == nil {
			L.Push(lua.LFalse)
			return 1
		}
		playerRID := uint32(L.CheckNumber(1))
		itemID := uint16(L.CheckNumber(2))
		qty := L.OptInt(3, 1)
		ok, err := r.inventory.RemoveItem(playerRID, itemID, qty)
		if err != nil {
			log.Printf("scripting: inventory remove_item failed player=%d item=%d err=%v", playerRID, itemID, err)
			L.Push(lua.LFalse)
			return 1
		}
		L.Push(lua.LBool(ok))
		return 1
	}))

	// Inventory.add_item(player_id, item_id [, qty=1]) -> ok(bool)
	r.L.SetField(mod, "add_item", r.L.NewFunction(func(L *lua.LState) int {
		if r.inventory == nil {
			L.Push(lua.LFalse)
			return 1
		}
		playerRID := uint32(L.CheckNumber(1))
		itemID := uint16(L.CheckNumber(2))
		qty := L.OptInt(3, 1)
		ok, err := r.inventory.AddItem(playerRID, itemID, qty)
		if err != nil {
			log.Printf("scripting: inventory add_item failed player=%d item=%d err=%v", playerRID, itemID, err)
			L.Push(lua.LFalse)
			return 1
		}
		L.Push(lua.LBool(ok))
		return 1
	}))

	r.L.SetGlobal("Inventory", mod)
}

// ---------------------------------------------------------------------------
// Timer API  —  Timer.after(seconds, fn)
//
// One-shot delayed callback, e.g. "close the gate 20s after it opens".
// Backed by Area.ScheduleCall (world/area.go — checked once per ~100ms AI
// tick, so not millisecond-precise, fine for gameplay timers). The area is
// captured from r.ctx.area at SCHEDULE time (same area the script is running
// in right now); the callback re-enters the Lua state on its own with a
// fresh callCtx when it eventually fires, mirroring how InteractNPC/
// ObjectInteract/etc already set up r.ctx before calling into Lua.
// ---------------------------------------------------------------------------

func (r *Registry) registerTimerAPI() {
	mod := r.L.NewTable()

	// Timer.after(seconds, function() ... end)
	r.L.SetField(mod, "after", r.L.NewFunction(func(L *lua.LState) int {
		seconds := float64(L.CheckNumber(1))
		fn := L.CheckFunction(2)
		area := r.ctx.area
		if area == nil || seconds < 0 {
			return 0
		}
		delayMs := int64(seconds * 1000)
		area.ScheduleCall(delayMs, func() {
			r.mu.Lock()
			defer r.mu.Unlock()
			r.ctx = callCtx{area: area}
			if err := r.safeCall(fn); err != nil {
				log.Printf("scripting: timer callback: %v", err)
			}
			r.ctx = callCtx{}
		})
		return 0
	}))

	r.L.SetGlobal("Timer", mod)
}

func (r *Registry) registerLogAPI() {
	r.L.SetGlobal("Log", r.L.NewFunction(func(L *lua.LState) int {
		msg := L.CheckString(1)
		log.Printf("scripting: [lua] %s", msg)
		return 0
	}))
}

// ---------------------------------------------------------------------------
// Time API  —  Time.now_ms()
//
// os.time() is unavailable to scripts (os/io are stripped globally in
// Registry.New — sandboxing scripts away from the filesystem/process), so
// this is the only clock scripts have. Wraps the same nowMs() helper used
// server-side for cooldown bookkeeping (e.g. Actor.LastCombatAt), letting a
// script keep its own per-player cooldown table (player_id -> last-used
// timestamp) entirely in Lua, with no new engine-side cooldown support.
// ---------------------------------------------------------------------------

func (r *Registry) registerTimeAPI() {
	mod := r.L.NewTable()
	r.L.SetField(mod, "now_ms", r.L.NewFunction(func(L *lua.LState) int {
		L.Push(lua.LNumber(nowMs()))
		return 1
	}))
	r.L.SetGlobal("Time", mod)
}

// ---------------------------------------------------------------------------
// FX API  —  FX.play(fx_key, x, y, z, magnitude)
//
// Thin bridge to world.BroadcastFXAtPosition, which reuses the EXACT same
// abilityFXHook every combat VFX (windup/impact/blood) already goes
// through — no new client-side handling needed, PCreateEmitter already
// consumes an arbitrary fx_key + position with no combat context
// requirement. See docs/TECH_DEBT.md, FX.play + SpawnTempProp
// investigation.
// ---------------------------------------------------------------------------

func (r *Registry) registerFXAPI() {
	mod := r.L.NewTable()

	// FX.play(fx_key, x, y, z [, magnitude]) — fires a fx_templates VFX
	// (see media/fx_templates, same catalog Attack/Death/spell FX use) at
	// an arbitrary world position. No caster/target/ability context —
	// pure "play this effect here."
	r.L.SetField(mod, "play", r.L.NewFunction(func(L *lua.LState) int {
		if r.ctx.area == nil {
			return 0
		}
		fxKey := L.CheckString(1)
		x := float32(L.CheckNumber(2))
		y := float32(L.CheckNumber(3))
		z := float32(L.CheckNumber(4))
		magnitude := float32(L.OptNumber(5, 1.0))
		world.BroadcastFXAtPosition(r.ctx.area, fxKey, x, y, z, magnitude)
		return 0
	}))

	r.L.SetGlobal("FX", mod)
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
