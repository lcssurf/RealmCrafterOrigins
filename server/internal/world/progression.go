package world

import (
	"math"
	"strings"
	"sync"
)

// ---------------------------------------------------------------------------
// Progression tables — isolate here so they can be ported to Lua/GUE later.
// ---------------------------------------------------------------------------

const MaxLevel = 60

// CharacterProgressionRuntimeConfig is the runtime copy of the DB progression
// config. It is loaded at startup so world code does not import db.
type CharacterProgressionRuntimeConfig struct {
	MaxLevel             int
	XPCurveType          string
	XPCurveBase          int
	XPCurveExponent      float64
	XPIrregularity       float64
	StatPointsPerLevel   int
	InitialStatValue     int
	RespecFreeUntilLevel int
	RespecCostGold       int
}

var (
	charProgressionMu     sync.RWMutex
	charProgressionConfig = defaultCharacterProgressionConfig()
)

func defaultCharacterProgressionConfig() CharacterProgressionRuntimeConfig {
	return CharacterProgressionRuntimeConfig{
		MaxLevel:             MaxLevel,
		XPCurveType:          "irregular",
		XPCurveBase:          60,
		XPCurveExponent:      2.5,
		XPIrregularity:       0.4,
		StatPointsPerLevel:   5,
		InitialStatValue:     5,
		RespecFreeUntilLevel: 10,
		RespecCostGold:       1000,
	}
}

// LoadAndCacheCharacterProgressionConfig replaces the runtime character XP
// curve. Call once on server startup after DB seeding/migrations.
func LoadAndCacheCharacterProgressionConfig(cfg CharacterProgressionRuntimeConfig) {
	cfg = normalizeCharacterProgressionConfig(cfg)
	charProgressionMu.Lock()
	charProgressionConfig = cfg
	charProgressionMu.Unlock()
}

func getCachedCharProgressionConfig() CharacterProgressionRuntimeConfig {
	charProgressionMu.RLock()
	defer charProgressionMu.RUnlock()
	return charProgressionConfig
}

// GetCachedCharProgressionConfig returns the normalized runtime copy of the
// character progression configuration loaded at startup.
func GetCachedCharProgressionConfig() CharacterProgressionRuntimeConfig {
	return getCachedCharProgressionConfig()
}

func normalizeCharacterProgressionConfig(cfg CharacterProgressionRuntimeConfig) CharacterProgressionRuntimeConfig {
	if cfg.MaxLevel < 1 {
		cfg.MaxLevel = MaxLevel
	}
	if cfg.XPCurveBase <= 0 {
		cfg.XPCurveBase = 60
	}
	if cfg.XPCurveExponent <= 0 {
		cfg.XPCurveExponent = 2.5
	}
	if cfg.XPIrregularity < 0 {
		cfg.XPIrregularity = 0
	}
	if cfg.XPIrregularity > 1 {
		cfg.XPIrregularity = 1
	}
	if cfg.StatPointsPerLevel < 0 {
		cfg.StatPointsPerLevel = 0
	}
	if cfg.InitialStatValue < 1 {
		cfg.InitialStatValue = 1
	}
	if cfg.RespecFreeUntilLevel < 0 {
		cfg.RespecFreeUntilLevel = 0
	}
	if cfg.RespecCostGold < 0 {
		cfg.RespecCostGold = 0
	}
	cfg.XPCurveType = strings.ToLower(strings.TrimSpace(cfg.XPCurveType))
	switch cfg.XPCurveType {
	case "irregular", "linear", "quadratic", "exponential":
	default:
		cfg.XPCurveType = "irregular"
	}
	return cfg
}

// MaxCharacterLevel returns the configured character level cap.
func MaxCharacterLevel() int {
	return getCachedCharProgressionConfig().MaxLevel
}

// ComputeXPThreshold returns the cumulative XP required to reach level.
// Level 1 always requires 0 XP.
func ComputeXPThreshold(level int, curveType string, base int, exponent float64, irregularity float64) int64 {
	if level <= 1 {
		return 0
	}
	if base <= 0 {
		base = 1
	}
	if exponent <= 0 {
		exponent = 1
	}
	if irregularity < 0 {
		irregularity = 0
	}
	if irregularity > 1 {
		irregularity = 1
	}

	curveType = strings.ToLower(strings.TrimSpace(curveType))
	l := float64(level)
	switch curveType {
	case "irregular":
		baseValue := float64(base) * math.Pow(l, exponent)
		jitter := (float64((level*73+37)%100) / 100.0) * irregularity
		return int64(math.Round(baseValue * (1.0 + jitter)))
	case "exponential":
		return int64(math.Round(float64(base) * math.Pow(1.5, l-1)))
	case "quadratic":
		n := int64(level - 1)
		return int64(base) * n * n
	case "linear":
		return int64(base) * int64(level-1)
	default:
		return int64(base) * int64(level-1)
	}
}

// XPToLevel returns the total XP required to reach the given level (1-based).
// Level 1 always requires 0 XP.
func XPToLevel(level int) int64 {
	cfg := getCachedCharProgressionConfig()
	return ComputeXPThreshold(level, cfg.XPCurveType, cfg.XPCurveBase, cfg.XPCurveExponent, cfg.XPIrregularity)
}

// XPForKill returns the XP reward for killing an NPC of the given level.
func XPForKill(npcLevel int) int64 {
	cfg := GetKillXPScalingConfig()
	return int64(npcLevel) * int64(cfg.BaseXPPerNPCLevel)
}

// ProcessXP applies an XP gain to a character and returns the updated values.
// Returns (newXP, newLevel, leveled).
func ProcessXP(currentXP int64, currentLevel int, gain int64) (newXP int64, newLevel int, leveled bool) {
	newXP = currentXP + gain
	newLevel = currentLevel
	for newLevel < MaxCharacterLevel() && newXP >= XPToLevel(newLevel+1) {
		newLevel++
		leveled = true
	}
	return
}

// ProcessXPCumulative adds gain to cumulative XP and updates level if absolute
// thresholds are crossed. Returns (newXP, newLevel, leveled).
func ProcessXPCumulative(currentXP int64, currentLevel int, gain int64) (newXP int64, newLevel int, leveled bool) {
	newXP = currentXP + gain
	if newXP < 0 {
		newXP = 0
	}
	newLevel = currentLevel
	if newLevel < 1 {
		newLevel = 1
	}

	for newLevel < MaxCharacterLevel() {
		nextThreshold := XPToLevel(newLevel + 1)
		if newXP < nextThreshold {
			break
		}
		newLevel++
		leveled = true
	}
	return
}

// Deprecated: use ProcessXPCumulative. This exists for compatibility with
// old save-data conversion paths.
// ProcessXPSinceLevel applies XP gain when XP is stored as "since last level".
// Returns (newXP, newLevel, leveled).
func ProcessXPSinceLevel(currentXP int64, currentLevel int, gain int64) (newXP int64, newLevel int, leveled bool) {
	if currentLevel < 1 {
		currentLevel = 1
	}
	if currentXP < 0 {
		currentXP = 0
	}
	if gain <= 0 {
		return currentXP, currentLevel, false
	}

	newXP = currentXP + gain
	newLevel = currentLevel

	for newLevel < MaxCharacterLevel() {
		curThreshold := XPToLevel(newLevel)
		nextThreshold := XPToLevel(newLevel + 1)
		delta := nextThreshold - curThreshold
		if delta <= 0 {
			break
		}
		if newXP < delta {
			break
		}
		newXP -= delta
		newLevel++
		leveled = true
	}
	return
}
