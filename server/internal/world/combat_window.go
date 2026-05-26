package world

import (
	"fmt"
	"strings"
	"sync"
	"time"
)

// CombatWindow tracks one player-vs-mob combat window used for mastery XP on kill.
// Key semantics:
//   - one window per (playerRID, mobRID)
//   - skills are tracked as a set of ability IDs
//   - KillingSkill stores the last tracked ability used by that player on that mob
type CombatWindow struct {
	PlayerRID      uint32
	PlayerCharID   string
	MobRID         uint32
	MobLevel       int32
	StartedAt      int64
	LastActivityAt int64
	SkillsUsed     map[uint32]bool
	KillingSkill   uint32
}

type CombatWindowManager struct {
	mu      sync.RWMutex
	windows map[string]*CombatWindow
}

var globalCombatWindows = &CombatWindowManager{
	windows: make(map[string]*CombatWindow),
}

func GetCombatWindowManager() *CombatWindowManager {
	return globalCombatWindows
}

func (m *CombatWindowManager) windowKey(playerRID, mobRID uint32) string {
	return fmt.Sprintf("%d-%d", playerRID, mobRID)
}

// EnsureWindow returns an existing window or creates one when missing.
func (m *CombatWindowManager) EnsureWindow(playerRID, mobRID uint32, mobLevel int32, charID string) *CombatWindow {
	if m == nil || playerRID == 0 || mobRID == 0 {
		return nil
	}

	now := time.Now().UnixMilli()
	charID = strings.TrimSpace(charID)

	m.mu.Lock()
	defer m.mu.Unlock()

	key := m.windowKey(playerRID, mobRID)
	w := m.windows[key]
	if w == nil {
		w = &CombatWindow{
			PlayerRID:      playerRID,
			PlayerCharID:   charID,
			MobRID:         mobRID,
			MobLevel:       mobLevel,
			StartedAt:      now,
			LastActivityAt: now,
			SkillsUsed:     make(map[uint32]bool),
		}
		m.windows[key] = w
		return w
	}

	w.LastActivityAt = now
	if mobLevel > 0 {
		w.MobLevel = mobLevel
	}
	if w.PlayerCharID == "" && charID != "" {
		w.PlayerCharID = charID
	}
	return w
}

// TrackSkill records one ability usage inside the player-vs-mob combat window.
func (m *CombatWindowManager) TrackSkill(playerRID, mobRID uint32, abilityID uint32, mobLevel int32, charID string) {
	if m == nil || playerRID == 0 || mobRID == 0 || abilityID == 0 {
		return
	}
	w := m.EnsureWindow(playerRID, mobRID, mobLevel, charID)
	if w == nil {
		return
	}

	now := time.Now().UnixMilli()
	m.mu.Lock()
	w.SkillsUsed[abilityID] = true
	w.KillingSkill = abilityID
	w.LastActivityAt = now
	m.mu.Unlock()
}

// CloseAllWindowsForMob closes and returns all windows that reference mobRID.
func (m *CombatWindowManager) CloseAllWindowsForMob(mobRID uint32) []*CombatWindow {
	if m == nil || mobRID == 0 {
		return nil
	}

	m.mu.Lock()
	defer m.mu.Unlock()

	closed := make([]*CombatWindow, 0)
	toDelete := make([]string, 0)
	for key, w := range m.windows {
		if w == nil || w.MobRID != mobRID {
			continue
		}
		closed = append(closed, w)
		toDelete = append(toDelete, key)
	}
	for _, key := range toDelete {
		delete(m.windows, key)
	}
	return closed
}

// CleanupExpired removes stale windows (no activity for timeoutMs).
func (m *CombatWindowManager) CleanupExpired(timeoutMs int64) {
	if m == nil || timeoutMs <= 0 {
		return
	}

	now := time.Now().UnixMilli()
	m.mu.Lock()
	for key, w := range m.windows {
		if w == nil || now-w.LastActivityAt > timeoutMs {
			delete(m.windows, key)
		}
	}
	m.mu.Unlock()
}

// CloseAllForPlayer removes all windows for one player runtime id.
func (m *CombatWindowManager) CloseAllForPlayer(playerRID uint32) {
	if m == nil || playerRID == 0 {
		return
	}
	m.mu.Lock()
	for key, w := range m.windows {
		if w != nil && w.PlayerRID == playerRID {
			delete(m.windows, key)
		}
	}
	m.mu.Unlock()
}
