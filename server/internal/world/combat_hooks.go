package world

import "sync"

type specialKillHookFn func(area *Area, attacker *Actor, target *Actor)
type specialHitHookFn func(area *Area, attacker *Actor, target *Actor, abilityID int)

var (
	specialKillHookMu sync.RWMutex
	specialKillHook   specialKillHookFn

	specialHitHookMu sync.RWMutex
	specialHitHook   specialHitHookFn
)

// SetSpecialKillHook registers an optional callback invoked when a special
// ability impact kills its target.
func SetSpecialKillHook(h specialKillHookFn) {
	specialKillHookMu.Lock()
	specialKillHook = h
	specialKillHookMu.Unlock()
}

// SetSpecialHitHook registers an optional callback invoked when a special
// ability impact hits successfully.
func SetSpecialHitHook(h specialHitHookFn) {
	specialHitHookMu.Lock()
	specialHitHook = h
	specialHitHookMu.Unlock()
}

func runSpecialKillHook(area *Area, attacker *Actor, target *Actor) {
	specialKillHookMu.RLock()
	h := specialKillHook
	specialKillHookMu.RUnlock()
	if h == nil {
		return
	}
	h(area, attacker, target)
}

func runSpecialHitHook(area *Area, attacker *Actor, target *Actor, abilityID int) {
	specialHitHookMu.RLock()
	h := specialHitHook
	specialHitHookMu.RUnlock()
	if h == nil {
		return
	}
	h(area, attacker, target, abilityID)
}
