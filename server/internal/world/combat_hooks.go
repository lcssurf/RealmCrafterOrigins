package world

import "sync"

type specialKillHookFn func(area *Area, attacker *Actor, target *Actor)

var (
	specialKillHookMu sync.RWMutex
	specialKillHook   specialKillHookFn
)

// SetSpecialKillHook registers an optional callback invoked when a special
// ability impact kills its target.
func SetSpecialKillHook(h specialKillHookFn) {
	specialKillHookMu.Lock()
	specialKillHook = h
	specialKillHookMu.Unlock()
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
