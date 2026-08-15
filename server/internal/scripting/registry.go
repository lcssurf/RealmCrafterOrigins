// Package scripting embeds a Lua 5.1 interpreter (gopher-lua) and exposes
// a modular event/spell API to game scripts.
//
// Architecture:
//   - One Lua state, mutex-protected (safe for concurrent Go goroutines).
//   - Scripts live in scripts/server/**/*.lua and are loaded at startup.
//   - Scripts register spells via Spell.define() + Spell.register().
//   - Scripts register event handlers via Event.on(name, fn).
//   - Go calls Registry.Cast() for spells and Registry.FireEvent() for events.
//   - All game-state mutation (damage, heal, HP packets) happens through the
//     Go API bindings — scripts never touch actors directly.
package scripting

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"

	lua "github.com/yuin/gopher-lua"
	"realm-crafter/server/internal/world"
)

// SpellDef holds the static definition of a spell registered from Lua.
type SpellDef struct {
	ID               uint16
	Name             string
	SpellType        string // "damage" | "heal" | "buff" | "debuff"
	EPCost           int32
	CooldownMs       int64
	Range            float32
	Icon             uint8
	AoEType          uint8          // 0=single 1=around_target 2=ground_target
	AoERadius        float32        // world units; 0 = not AoE
	RuntimeAbilityID int            // 0 = legacy spell path, >0 = cast_intent ability id
	onCast           *lua.LFunction // Lua function(caster_id, target_id)
}

// DialogPending holds a dialog the Lua script wants to send to the player.
type DialogPending struct {
	Text     string
	Options  []string
	OpenShop bool // if true, server should open the NPC's shop instead of a dialog
}

// callCtx is populated for the duration of a single Lua call.
type callCtx struct {
	area          *world.Area
	caster        *world.Actor
	killedRID     uint32         // set by deal_damage if target dies
	pendingDialog *DialogPending // set by Dialog.send
	// AoE context — set by Cast before calling Lua
	aoeType   uint8
	aoeRadius float32
	groundX   float32 // valid when aoeType == 2
	groundZ   float32
}

// Quest objective type constants mirrored for Lua bindings.
const (
	QuestObjectiveKill     uint8 = 1
	QuestObjectiveCollect  uint8 = 2
	QuestObjectiveTalk     uint8 = 3
	QuestObjectiveExplore  uint8 = 4
	QuestObjectiveInteract uint8 = 5
)

// QuestProgressEvent describes one objective progress event emitted by Lua.
type QuestProgressEvent struct {
	ObjectiveType uint8
	TargetNPCName string
	TargetItemID  uint16
	TargetArea    string
	Delta         int
}

// PlayerCastIntentAdvice is optional script advice applied to a player cast
// intent before it enters the authoritative world runtime.
type PlayerCastIntentAdvice struct {
	ActionOverride string
	ReasonTag      string
	ClientTraceID  string
	Cancel         bool
}

// QuestBridge is implemented by the game server runtime and injected into
// scripting so Lua can drive quest state without importing net/db packages.
type QuestBridge interface {
	Accept(playerRID uint32, questID int) (bool, error)
	Abandon(playerRID uint32, questID int) (bool, error)
	TurnIn(playerRID uint32, questID int) (bool, error)
	Progress(playerRID uint32, event QuestProgressEvent) (bool, error)
	Sync(playerRID uint32) error
}

// Registry owns the Lua state and all script-registered data.
type Registry struct {
	mu     sync.Mutex
	L      *lua.LState
	spells map[uint16]*SpellDef
	events map[string][]*lua.LFunction
	w      *world.World
	quest  QuestBridge
	inventory InventoryBridge
	globals GlobalsBridge
	ctx    callCtx
}

// New creates a Registry wired to the given world.
// Call LoadDir after creation to load scripts.
func New(w *world.World) *Registry {
	L := lua.NewState(lua.Options{
		// Keep standard libs but strip os/io so scripts can't access the filesystem.
		SkipOpenLibs: false,
	})

	r := &Registry{
		L:      L,
		spells: make(map[uint16]*SpellDef),
		events: make(map[string][]*lua.LFunction),
		w:      w,
	}

	r.registerAPI()

	// Remove dangerous stdlib modules.
	for _, mod := range []string{"io", "os"} {
		L.SetGlobal(mod, lua.LNil)
	}

	return r
}

// Close shuts down the Lua state.
func (r *Registry) Close() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.L.Close()
}

// SetQuestBridge injects runtime quest callbacks used by the Lua Quest API.
func (r *Registry) SetQuestBridge(quest QuestBridge) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.quest = quest
}

// LoadDir loads every *.lua file found recursively under dir.
// Errors in individual scripts are logged but do not stop loading.
func (r *Registry) LoadDir(dir string) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	return filepath.WalkDir(dir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() || filepath.Ext(path) != ".lua" {
			return nil
		}
		if loadErr := r.L.DoFile(path); loadErr != nil {
			log.Printf("scripting: load %s: %v", path, loadErr)
		} else {
			log.Printf("scripting: loaded %s", path)
		}
		return nil
	})
}

// GetSpell returns a SpellDef by ID, or nil if not found.
func (r *Registry) GetSpell(id uint16) *SpellDef {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.spells[id]
}

// AllSpells returns all registered spells sorted by ID.
func (r *Registry) AllSpells() []*SpellDef {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]*SpellDef, 0, len(r.spells))
	for _, s := range r.spells {
		out = append(out, s)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// Cast executes the on_cast Lua handler for the given spell.
// EP deduction and cooldown bookkeeping must be done by the caller before Cast.
// groundX/Z are used when def.AoEType == 2 (ground-targeted AoE); ignored otherwise.
// Returns the runtime ID of any actor killed by this cast (0 = none).
func (r *Registry) Cast(def *SpellDef, caster *world.Actor, target *world.Actor, area *world.Area, groundX, groundZ float32) uint32 {
	r.mu.Lock()
	defer r.mu.Unlock()

	if def.onCast == nil {
		return 0
	}

	r.ctx = callCtx{
		area:      area,
		caster:    caster,
		aoeType:   def.AoEType,
		aoeRadius: def.AoERadius,
		groundX:   groundX,
		groundZ:   groundZ,
	}

	targetArg := lua.LNumber(0)
	if target != nil {
		targetArg = lua.LNumber(target.RuntimeID)
	}

	if err := r.L.CallByParam(lua.P{
		Fn:      def.onCast,
		NRet:    0,
		Protect: true,
	}, lua.LNumber(caster.RuntimeID), targetArg); err != nil {
		log.Printf("scripting: spell %d on_cast: %v", def.ID, err)
	}

	killed := r.ctx.killedRID
	r.ctx = callCtx{}
	return killed
}

// FireEvent invokes all Lua handlers registered for eventName.
// args must be Lua-compatible types (lua.LValue, or Go primitives auto-converted).
func (r *Registry) FireEvent(eventName string, args ...lua.LValue) {
	r.mu.Lock()
	defer r.mu.Unlock()

	handlers, ok := r.events[eventName]
	if !ok {
		return
	}
	for _, fn := range handlers {
		if err := r.L.CallByParam(lua.P{
			Fn:      fn,
			NRet:    0,
			Protect: true,
		}, args...); err != nil {
			log.Printf("scripting: event %s handler: %v", eventName, err)
		}
	}
}

// safeCall calls a Lua function with protect=true and logs errors.
func (r *Registry) safeCall(fn *lua.LFunction, args ...lua.LValue) error {
	return r.L.CallByParam(lua.P{Fn: fn, NRet: 0, Protect: true}, args...)
}

// dispatchEntityScript is the shared "specific script, else generic
// fallback" firing logic behind every fire-and-forget entity dispatch
// (InteractNPC/HandleChoice/ObjectInteract/HandleObjectChoice/
// ItemUseScript/DispatchPlayerAction/DispatchAreaEnter/DispatchAreaExit/
// DispatchTriggerEnter/DispatchTriggerExit/DispatchAbilityCast/
// DispatchNPCSpawn) — generic scripting Fase 2, see docs/TECH_DEBT.md.
// Eliminates the identical "look up r.events[name], range, safeCall, log
// on error" block that was copy-pasted across all of them.
//
// specificEvent, when non-empty (the entity has its own script configured
// — e.g. ability_templates.on_cast_script, npc_spawns.spawn_script), is
// fired ALONE, in place of fallbackEvent — NOT in addition to it. An
// entity with no specific script configured (specificEvent=="", the
// default/legacy case for every entity that predates this field) always
// falls through to fallbackEvent, exactly reproducing today's behavior —
// see docs/TECH_DEBT.md's "confirmed: zero behavior change" note.
//
// Caller must already hold r.mu and have set up r.ctx if the handlers need
// area/caster/pendingDialog context (same pre-condition every refactored
// call site already had before this helper existed).
func (r *Registry) dispatchEntityScript(specificEvent, fallbackEvent, logLabel string, args ...lua.LValue) {
	eventName := fallbackEvent
	if specificEvent != "" {
		eventName = specificEvent
	}
	handlers, ok := r.events[eventName]
	if !ok {
		return
	}
	for _, fn := range handlers {
		if err := r.safeCall(fn, args...); err != nil {
			log.Printf("scripting: %s handler: %v", logLabel, err)
		}
	}
}

// nowMs returns current time in unix milliseconds.
func nowMs() int64 { return time.Now().UnixMilli() }

// spellTypeIndex converts a SpellType string to the uint8 sent in PKnownSpells.
func SpellTypeIndex(t string) uint8 {
	switch t {
	case "heal":
		return 1
	case "buff":
		return 2
	case "debuff":
		return 3
	default:
		return 0
	}
}

// InteractNPC fires the "npc_interact" event and returns any dialog the script queued.
func (r *Registry) InteractNPC(player, npc *world.Actor, area *world.Area) *DialogPending {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ctx = callCtx{area: area, caster: player}
	r.dispatchEntityScript("", "npc_interact", "npc_interact",
		lua.LNumber(player.RuntimeID), lua.LNumber(npc.RuntimeID))
	pending := r.ctx.pendingDialog
	r.ctx = callCtx{}
	return pending
}

// HandleChoice fires the "npc_choice" event and returns any follow-up dialog.
func (r *Registry) HandleChoice(player, npc *world.Actor, area *world.Area, choice uint8) *DialogPending {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ctx = callCtx{area: area, caster: player}
	r.dispatchEntityScript("", "npc_choice", "npc_choice",
		lua.LNumber(player.RuntimeID), lua.LNumber(npc.RuntimeID), lua.LNumber(choice))
	pending := r.ctx.pendingDialog
	r.ctx = callCtx{}
	return pending
}

// ObjectInteract fires the "object_interact" event and returns any dialog the
// script queued. Mirrors InteractNPC exactly, but the target is a placed
// WorldObject (identified by its zone_scenery ID) instead of an Actor/NPC.
func (r *Registry) ObjectInteract(player *world.Actor, objectID int, area *world.Area) *DialogPending {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ctx = callCtx{area: area, caster: player}
	r.dispatchEntityScript("", "object_interact", "object_interact",
		lua.LNumber(player.RuntimeID), lua.LNumber(objectID))
	pending := r.ctx.pendingDialog
	r.ctx = callCtx{}
	return pending
}

// HandleObjectChoice fires the "object_choice" event and returns any
// follow-up dialog. Mirrors HandleChoice, but for a WorldObject dialog.
func (r *Registry) HandleObjectChoice(player *world.Actor, objectID int, area *world.Area, choice uint8) *DialogPending {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ctx = callCtx{area: area, caster: player}
	r.dispatchEntityScript("", "object_choice", "object_choice",
		lua.LNumber(player.RuntimeID), lua.LNumber(objectID), lua.LNumber(choice))
	pending := r.ctx.pendingDialog
	r.ctx = callCtx{}
	return pending
}

// ItemUseScript fires the "item_use_script" event when a Script Item
// (item_templates.item_type == 4) is used. Its on-use effect is defined
// entirely in Lua (unlike Consumable/item_type==2, whose heal effect is
// hardcoded in UseItem/handleUseItem) — this is the generic path for keys,
// buffs, teleport scrolls, or any other on-use behavior. Mirrors
// InteractNPC's context setup; unlike InteractNPC/ObjectInteract, no dialog
// is expected back — a door-key use case would typically drive its dialog
// off object_interact/object_choice instead (see Inventory.has_item/
// Inventory.remove_item), this event exists for scripts that want to react
// to "player used this script item" directly.
func (r *Registry) ItemUseScript(player *world.Actor, itemID uint16, area *world.Area) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ctx = callCtx{area: area, caster: player}
	r.dispatchEntityScript("", "item_use_script", "item_use_script",
		lua.LNumber(player.RuntimeID), lua.LNumber(itemID))
	r.ctx = callCtx{}
}

// ItemUseScriptOnTarget is the dual-mode "item used ON another entity"
// path (generic scripting Fase 2, item 2) — the RC-style tool-used-on-a-
// resource-node case (harvesting being the motivating example). Distinct
// from ItemUseScript (no target) rather than a variant of it: an item
// meant to be used standalone (a scroll, a key) and an item meant to be
// aimed at something (a pickaxe) are different interactions with different
// expected Lua signatures — merging them behind one event with an
// optional/zero targetID would force every handler to branch on whether
// target_id==0 meant "no target" or "targeted actor 0" (impossible RID,
// but still a footgun). scriptOnTarget is item_templates.script_on_target,
// resolved by the caller (net/client.go's handleUseItem via
// db.UseItemResult) — scripting package doesn't import db.
func (r *Registry) ItemUseScriptOnTarget(player *world.Actor, itemID uint16, targetID uint32, scriptOnTarget string, area *world.Area) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ctx = callCtx{area: area, caster: player}
	r.dispatchEntityScript(scriptOnTarget, "item_use_on_target", "item_use_on_target",
		lua.LNumber(player.RuntimeID), lua.LNumber(itemID), lua.LNumber(targetID))
	r.ctx = callCtx{}
}

// InventoryBridge is implemented by the game server runtime and injected
// into scripting so Lua can query/consume inventory items without importing
// net/db packages. Mirrors QuestBridge.
type InventoryBridge interface {
	HasItem(playerRID uint32, itemID uint16, qty int) (bool, error)
	RemoveItem(playerRID uint32, itemID uint16, qty int) (bool, error)
	AddItem(playerRID uint32, itemID uint16, qty int) (bool, error)
}

// SetInventoryBridge injects runtime inventory callbacks used by the Lua
// Inventory API. Mirrors SetQuestBridge.
func (r *Registry) SetInventoryBridge(inv InventoryBridge) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.inventory = inv
}

// GlobalsBridge is implemented by the game server runtime and injected into
// scripting so Lua can persist free-form content/script data without
// importing net/db packages. Mirrors QuestBridge/InventoryBridge.
//
// This is explicitly a CONTENT/SCRIPT data store (quest flags, one-off
// world state, "has this player seen the intro cutscene" type things) —
// NEVER for real combat stats. Combat-relevant numbers (HP, Derived, item
// attributes, etc.) stay in their typed structs/columns, where the type
// system and the rest of the combat pipeline can actually reason about
// them; a stringly-typed KV store has no place computing damage. See
// docs/TECH_DEBT.md, generic scripting Fase 1.
type GlobalsBridge interface {
	SetActorGlobal(playerRID uint32, key, value string) error
	GetActorGlobal(playerRID uint32, key string) (string, error)
	SetWorldGlobal(areaName, key, value string) error
	GetWorldGlobal(areaName, key string) (string, error)
}

// SetGlobalsBridge injects runtime key-value callbacks used by the Lua
// Actor.set_global/get_global and World.set_global/get_global API. Mirrors
// SetQuestBridge/SetInventoryBridge.
func (r *Registry) SetGlobalsBridge(g GlobalsBridge) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.globals = g
}

// PatchAoEFromDB overlays aoe_type and aoe_radius from DB rows onto in-memory SpellDefs.
// Call this after LoadDir so GUE edits take effect without changing Lua scripts.
type SpellAoERow struct {
	ID        uint16
	AoEType   uint8
	AoERadius float32
}

func (r *Registry) PatchAoEFromDB(rows []SpellAoERow) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, row := range rows {
		if def, ok := r.spells[row.ID]; ok {
			def.AoEType = row.AoEType
			def.AoERadius = row.AoERadius
		}
	}
}

// PatchRuntimeAbilityFromDB overlays runtime_ability_id from DB rows.
// 0 means this spell stays on legacy script execution path.
type SpellRuntimeAbilityRow struct {
	ID               uint16
	RuntimeAbilityID int
}

func (r *Registry) PatchRuntimeAbilityFromDB(rows []SpellRuntimeAbilityRow) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, row := range rows {
		if def, ok := r.spells[row.ID]; ok {
			if row.RuntimeAbilityID < 0 {
				row.RuntimeAbilityID = 0
			}
			def.RuntimeAbilityID = row.RuntimeAbilityID
		}
	}
}

// DispatchPlayerAction fires the "player_action" scripting event, allowing
// Lua scripts to react to player inputs forwarded from PPlayerAction.
// actor is the player who triggered the action; state is 0=press, 1=hold_start.
func (r *Registry) DispatchPlayerAction(actor *world.Actor, action string, state uint8) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.dispatchEntityScript("", "player_action", "player_action",
		lua.LNumber(actor.RuntimeID), lua.LString(action), lua.LNumber(state))
}

// DispatchAreaEnter fires the "area_enter" scripting event — generic
// scripting Fase 1 (see docs/TECH_DEBT.md). Called from net/client.go on
// login and after a portal completes. Mirrors DispatchPlayerAction exactly.
//
//	Event.on("area_enter", function(player_id, area_name) ... end)
func (r *Registry) DispatchAreaEnter(actor *world.Actor, areaName string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.dispatchEntityScript("", "area_enter", "area_enter",
		lua.LNumber(actor.RuntimeID), lua.LString(areaName))
}

// DispatchAreaExit fires the "area_exit" scripting event — see
// DispatchAreaEnter. Called from net/client.go BEFORE the actor leaves the
// old area (portal only — there's no "exit" on login, nothing to exit
// from).
//
//	Event.on("area_exit", function(player_id, area_name) ... end)
func (r *Registry) DispatchAreaExit(actor *world.Actor, areaName string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.dispatchEntityScript("", "area_exit", "area_exit",
		lua.LNumber(actor.RuntimeID), lua.LString(areaName))
}

// DispatchTriggerEnter fires the "trigger_enter" scripting event — generic
// scripting Fase 1/2. Called from world.SetZoneTriggerHook's implementation
// (net/zone_trigger_bridge.go), itself driven by Area.tickAreaTriggers.
//
// Fase 2 update: now DOES consult the trigger's own Trigger.Script field
// (area_triggers table, GUE-authorable since before this round — see
// Area.TriggerScriptByID) as the specific-script source — a trigger with
// Script set fires THAT event alone instead of the generic "trigger_enter"
// fallback. Every trigger authored before this round has Script=="" (the
// field existed but was never consumed until now), so this is a pure
// capability addition, not a behavior change for existing content.
//
//	Event.on("trigger_enter", function(player_id, trigger_id) ... end)
func (r *Registry) DispatchTriggerEnter(actor *world.Actor, triggerID int, area *world.Area) {
	r.mu.Lock()
	defer r.mu.Unlock()
	specific := ""
	if area != nil {
		specific = area.TriggerScriptByID(triggerID)
	}
	r.dispatchEntityScript(specific, "trigger_enter", "trigger_enter",
		lua.LNumber(actor.RuntimeID), lua.LNumber(triggerID))
}

// DispatchTriggerExit fires the "trigger_exit" scripting event — see
// DispatchTriggerEnter.
//
//	Event.on("trigger_exit", function(player_id, trigger_id) ... end)
func (r *Registry) DispatchTriggerExit(actor *world.Actor, triggerID int, area *world.Area) {
	r.mu.Lock()
	defer r.mu.Unlock()
	specific := ""
	if area != nil {
		specific = area.TriggerScriptByID(triggerID)
	}
	r.dispatchEntityScript(specific, "trigger_exit", "trigger_exit",
		lua.LNumber(actor.RuntimeID), lua.LNumber(triggerID))
}

// DispatchNPCSpawn fires the "npc_spawn" scripting event — generic
// scripting Fase 2 (see docs/TECH_DEBT.md, item 4). npc.SpawnScript
// (copied from npc_spawns.spawn_script at spawn time — main.go for the
// initial startup spawn, respawnNPC/area.go via world.SetNPCSpawnHook for
// every respawn) is the specific-script source: a resource-node spawn
// point can set its own event name to initialize per-instance state
// (durability, resource type) without a dedicated DB table per resource
// type — the motivating harvesting use case.
//
//	Event.on("npc_spawn", function(npc_id) ... end)
func (r *Registry) DispatchNPCSpawn(npc *world.Actor, area *world.Area) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ctx = callCtx{area: area}
	r.dispatchEntityScript(npc.SpawnScript, "npc_spawn", "npc_spawn", lua.LNumber(npc.RuntimeID))
	r.ctx = callCtx{}
}

// DispatchAbilityCast fires the "ability_cast" scripting event — generic
// scripting Fase 2 (item 3). Called from world.SetAbilityCastHook's
// implementation (net/ability_cast_bridge.go), itself driven by
// tryStartCastByRIDAt (cast_intent.go) on every successful cast start
// (player or NPC — same shared ability_templates row), AFTER the normal
// damage/CC/status-effect resolution pipeline already handles the
// ability's configured mechanical effects. ability_templates.on_cast_script
// is the specific-script source — lets one ability have fully custom
// behavior authored purely in Lua/GUE, no Go change required.
//
//	Event.on("ability_cast", function(caster_id, target_id, ability_id) ... end)
func (r *Registry) DispatchAbilityCast(caster, target *world.Actor, abilityID int, area *world.Area) {
	r.mu.Lock()
	defer r.mu.Unlock()
	ability, ok := world.GetAbilityTemplateByID(abilityID)
	if !ok {
		return
	}
	targetRID := uint32(0)
	if target != nil {
		targetRID = target.RuntimeID
	}
	r.ctx = callCtx{area: area, caster: caster}
	r.dispatchEntityScript(ability.OnCastScript, "ability_cast", "ability_cast",
		lua.LNumber(caster.RuntimeID), lua.LNumber(targetRID), lua.LNumber(abilityID))
	r.ctx = callCtx{}
}

// DispatchPlayerBeforeCastIntent fires the "player_before_cast_intent" event.
// Expected Lua signature:
//
//	function(player_id, target_id, ability_id, reason_tag)
//	  return {
//	    action_override = "AttackHeavy",
//	    reason_tag = "script_combo",
//	    client_trace_id = "trace-123",
//	    cancel = false
//	  }
//
// Any field may be omitted. Multiple handlers are merged in registration order.
func (r *Registry) DispatchPlayerBeforeCastIntent(
	area *world.Area,
	player *world.Actor,
	target *world.Actor,
	abilityID int,
	reasonTag string,
) PlayerCastIntentAdvice {
	advice := PlayerCastIntentAdvice{}
	if area == nil || player == nil || abilityID <= 0 {
		return advice
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	handlers, ok := r.events["player_before_cast_intent"]
	if !ok || len(handlers) == 0 {
		return advice
	}

	r.ctx = callCtx{area: area, caster: player}
	defer func() { r.ctx = callCtx{} }()

	playerRID := lua.LNumber(player.RuntimeID)
	targetRID := lua.LNumber(0)
	if target != nil {
		targetRID = lua.LNumber(target.RuntimeID)
	}
	abilityIDValue := lua.LNumber(abilityID)
	effectiveReason := reasonTag

	for _, fn := range handlers {
		if err := r.L.CallByParam(lua.P{
			Fn:      fn,
			NRet:    1,
			Protect: true,
		}, playerRID, targetRID, abilityIDValue, lua.LString(effectiveReason)); err != nil {
			log.Printf("scripting: player_before_cast_intent handler: %v", err)
			continue
		}

		ret := r.L.Get(-1)
		if tbl, ok := ret.(*lua.LTable); ok && tbl != nil {
			if v := luaStrField(tbl, "action_override", ""); v != "" {
				advice.ActionOverride = v
			}
			if v := luaStrField(tbl, "reason_tag", ""); v != "" {
				advice.ReasonTag = v
				effectiveReason = v
			}
			if v := luaStrField(tbl, "client_trace_id", ""); v != "" {
				advice.ClientTraceID = v
			}
			if bv, ok := tbl.RawGetString("cancel").(lua.LBool); ok && bool(bv) {
				advice.Cancel = true
			}
		}
		r.L.Pop(1)

		if advice.Cancel {
			break
		}
	}

	return advice
}

// DispatchNPCAIDecision fires the NPC scripted decision events.
// Supported event names:
//   - "npc_decide_ability" (new canonical name)
//   - "npc_ai_decide" (legacy name kept for compatibility)
//
// Expected Lua signature: function(npc_id, target_id, now_ms)
// Returns true when a handler started a special cast windup for this NPC tick.
func (r *Registry) DispatchNPCAIDecision(area *world.Area, npc, target *world.Actor, now int64) bool {
	if area == nil || npc == nil || target == nil {
		return false
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	handlers := make([]*lua.LFunction, 0, len(r.events["npc_decide_ability"])+len(r.events["npc_ai_decide"]))
	handlers = append(handlers, r.events["npc_decide_ability"]...)
	handlers = append(handlers, r.events["npc_ai_decide"]...)
	if len(handlers) == 0 {
		return false
	}

	r.ctx = callCtx{area: area, caster: npc}
	defer func() { r.ctx = callCtx{} }()

	npcRID := lua.LNumber(npc.RuntimeID)
	targetRID := lua.LNumber(target.RuntimeID)
	nowMsVal := lua.LNumber(now)

	for _, fn := range handlers {
		if err := r.safeCall(fn, npcRID, targetRID, nowMsVal); err != nil {
			log.Printf("scripting: npc decision handler: %v", err)
			continue
		}
		npc.Mu.Lock()
		started := false
		for _, p := range npc.PendingImpacts {
			if p.GatesAbilityCasts && p.ResolveAt > now {
				started = true
				break
			}
		}
		npc.Mu.Unlock()
		if started {
			return true
		}
	}
	return false
}

// DispatchChatCommand fires the generic "chat_command" scripting event —
// the fallback for any chat message starting with "/" that no native Go
// command handled (see handleSlashCommand in net/client.go). cmd is the
// lowercased command word without the leading slash (e.g. "unstuck"); args
// is everything after it, trimmed, as a single string (empty if none).
//
// Expected Lua signature:
//
//	Event.on("chat_command", function(player_id, command, args)
//	    if command == "unstuck" then
//	        -- ... handle it ...
//	        return true  -- consumed: don't broadcast "/unstuck" as chat text
//	    end
//	    return false  -- or omit: not handled by this script, try to broadcast raw
//	end)
//
// Returns true if ANY registered handler returned true (consumed) — the
// first such handler stops the loop, same short-circuit style as
// DispatchPlayerBeforeCastIntent's cancel flag. Multiple scripts can share
// this one event name; each checks `command` itself, exactly like a single
// Go switch/case would, just in Lua instead of native code.
func (r *Registry) DispatchChatCommand(area *world.Area, player *world.Actor, cmd, args string) bool {
	if area == nil || player == nil || cmd == "" {
		return false
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	handlers, ok := r.events["chat_command"]
	if !ok || len(handlers) == 0 {
		return false
	}

	r.ctx = callCtx{area: area, caster: player}
	defer func() { r.ctx = callCtx{} }()

	playerRID := lua.LNumber(player.RuntimeID)
	cmdVal := lua.LString(cmd)
	argsVal := lua.LString(args)

	for _, fn := range handlers {
		if err := r.L.CallByParam(lua.P{
			Fn:      fn,
			NRet:    1,
			Protect: true,
		}, playerRID, cmdVal, argsVal); err != nil {
			log.Printf("scripting: chat_command handler: %v", err)
			continue
		}
		ret := r.L.Get(-1)
		r.L.Pop(1)
		if b, ok := ret.(lua.LBool); ok && bool(b) {
			return true
		}
	}
	return false
}

// registerSpell is called from the Lua API to register a new SpellDef.
func (r *Registry) registerSpell(def *SpellDef) error {
	if def.ID == 0 {
		return fmt.Errorf("spell id must be > 0")
	}
	r.spells[def.ID] = def
	log.Printf("scripting: registered spell %d %q (%s)", def.ID, def.Name, def.SpellType)
	return nil
}
