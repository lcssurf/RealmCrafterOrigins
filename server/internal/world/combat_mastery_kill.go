package world

import (
	"log"
	"sort"
	"strings"
	"sync"
)

type NPCKilledMasteryHook func(playerCharID string, abilityID uint32, xp int64, isKillingBlow bool)

var (
	npcKilledMasteryHookMu sync.RWMutex
	npcKilledMasteryHook   NPCKilledMasteryHook
)

// SetNPCKilledMasteryHook registers an optional callback used to persist and
// push mastery XP after kill-window distribution is calculated in world.
func SetNPCKilledMasteryHook(h NPCKilledMasteryHook) {
	npcKilledMasteryHookMu.Lock()
	npcKilledMasteryHook = h
	npcKilledMasteryHookMu.Unlock()
}

func runNPCKilledMasteryHook(playerCharID string, abilityID uint32, xp int64, isKillingBlow bool) {
	npcKilledMasteryHookMu.RLock()
	h := npcKilledMasteryHook
	npcKilledMasteryHookMu.RUnlock()
	if h == nil {
		return
	}
	h(playerCharID, abilityID, xp, isKillingBlow)
}

// OnNPCKilled closes all combat windows for this mob and distributes mastery XP
// to tracked skills (special abilities only; melee basic is intentionally
// excluded by design).
func OnNPCKilled(_ *Area, npc *Actor, killerAttackerRID uint32) {
	if npc == nil || !npc.IsNPC || npc.RuntimeID == 0 {
		return
	}

	windows := GetCombatWindowManager().CloseAllWindowsForMob(npc.RuntimeID)
	if len(windows) == 0 {
		return
	}

	cfg := GetMasteryKillScalingConfig()
	baseXP := int64(npc.Level) * int64(cfg.XPPerMobLevel)
	if baseXP <= 0 {
		return
	}

	for _, win := range windows {
		if win == nil {
			continue
		}
		charID := strings.TrimSpace(win.PlayerCharID)
		if charID == "" || len(win.SkillsUsed) == 0 {
			continue
		}

		abilityIDs := make([]uint32, 0, len(win.SkillsUsed))
		for abilityID := range win.SkillsUsed {
			if abilityID > 0 {
				abilityIDs = append(abilityIDs, abilityID)
			}
		}
		if len(abilityIDs) == 0 {
			continue
		}
		sort.Slice(abilityIDs, func(i, j int) bool { return abilityIDs[i] < abilityIDs[j] })

		xpPerSkill := baseXP / int64(len(abilityIDs))
		if xpPerSkill < 1 {
			xpPerSkill = 1
		}

		for _, abilityID := range abilityIDs {
			xp := xpPerSkill
			isKillingBlow := killerAttackerRID != 0 &&
				win.PlayerRID == killerAttackerRID &&
				abilityID == win.KillingSkill
			if isKillingBlow {
				xp = int64(float64(xpPerSkill) * float64(cfg.KillingBlowMult))
				if xp < 1 {
					xp = 1
				}
			}
			runNPCKilledMasteryHook(charID, abilityID, xp, isKillingBlow)
			log.Printf("mastery-xp-kill: char=%s mob=%d ability=%d xp=%d killing_blow=%t",
				charID, npc.RuntimeID, abilityID, xp, isKillingBlow)
		}
	}
}
