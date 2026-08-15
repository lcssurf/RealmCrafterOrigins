package world

import "sync"

// NPCSpawnHook is implemented by the net package and invoked whenever an
// NPC appears in the world (respawn only — the initial startup spawn calls
// Registry.DispatchNPCSpawn directly from main.go, which already has both
// the *world.Area and the *scripting.Registry in scope; only respawnNPC,
// deep inside world's own tick loop, needs the hook indirection). Generic
// scripting Fase 2 — mirrors ZoneTriggerHook/SetStatusEffectHook's "world
// owns WHEN, net owns HOW to reach Lua" split.
type NPCSpawnHook func(area *Area, npc *Actor)

// AbilityCastHook is implemented by the net package and invoked whenever
// ANY cast (player or NPC) actually starts (tryStartCastByRIDAt,
// cast_intent.go) — after windup/GCD/cooldown/range/resource/weapon
// checks all pass, same single funnel every other Fase 1/2/3 cast-time
// check already goes through.
type AbilityCastHook func(area *Area, caster, target *Actor, abilityID int)

var (
	npcSpawnHookMu sync.RWMutex
	npcSpawnHook   NPCSpawnHook

	abilityCastHookMu sync.RWMutex
	abilityCastHook    AbilityCastHook
)

// SetNPCSpawnHook registers the callback invoked on every NPC respawn.
func SetNPCSpawnHook(h NPCSpawnHook) {
	npcSpawnHookMu.Lock()
	npcSpawnHook = h
	npcSpawnHookMu.Unlock()
}

func runNPCSpawnHook(area *Area, npc *Actor) {
	npcSpawnHookMu.RLock()
	h := npcSpawnHook
	npcSpawnHookMu.RUnlock()
	if h == nil {
		return
	}
	h(area, npc)
}

// SetAbilityCastHook registers the callback invoked whenever a cast starts.
func SetAbilityCastHook(h AbilityCastHook) {
	abilityCastHookMu.Lock()
	abilityCastHook = h
	abilityCastHookMu.Unlock()
}

func runAbilityCastHook(area *Area, caster, target *Actor, abilityID int) {
	abilityCastHookMu.RLock()
	h := abilityCastHook
	abilityCastHookMu.RUnlock()
	if h == nil {
		return
	}
	h(area, caster, target, abilityID)
}
