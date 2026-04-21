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
	ID         uint16
	Name       string
	SpellType  string // "damage" | "heal" | "buff" | "debuff"
	EPCost     int32
	CooldownMs int64
	Range      float32
	Icon       uint8
	AoEType    uint8   // 0=single 1=around_target 2=ground_target
	AoERadius  float32 // world units; 0 = not AoE
	onCast     *lua.LFunction // Lua function(caster_id, target_id)
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

// Registry owns the Lua state and all script-registered data.
type Registry struct {
	mu     sync.Mutex
	L      *lua.LState
	spells map[uint16]*SpellDef
	events map[string][]*lua.LFunction
	w      *world.World
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
	for _, fn := range r.events["npc_interact"] {
		if err := r.safeCall(fn, lua.LNumber(player.RuntimeID), lua.LNumber(npc.RuntimeID)); err != nil {
			log.Printf("scripting: npc_interact: %v", err)
		}
	}
	pending := r.ctx.pendingDialog
	r.ctx = callCtx{}
	return pending
}

// HandleChoice fires the "npc_choice" event and returns any follow-up dialog.
func (r *Registry) HandleChoice(player, npc *world.Actor, area *world.Area, choice uint8) *DialogPending {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ctx = callCtx{area: area, caster: player}
	for _, fn := range r.events["npc_choice"] {
		if err := r.safeCall(fn, lua.LNumber(player.RuntimeID), lua.LNumber(npc.RuntimeID), lua.LNumber(choice)); err != nil {
			log.Printf("scripting: npc_choice: %v", err)
		}
	}
	pending := r.ctx.pendingDialog
	r.ctx = callCtx{}
	return pending
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

// registerSpell is called from the Lua API to register a new SpellDef.
func (r *Registry) registerSpell(def *SpellDef) error {
	if def.ID == 0 {
		return fmt.Errorf("spell id must be > 0")
	}
	r.spells[def.ID] = def
	log.Printf("scripting: registered spell %d %q (%s)", def.ID, def.Name, def.SpellType)
	return nil
}
