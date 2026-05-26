package world

import "sync"

type KillXPScalingConfig struct {
	BaseXPPerNPCLevel    int32
	LevelDiffCoefficient float32
	MultiplierMin        float32
	MultiplierMax        float32
}

type MasteryKillScalingConfig struct {
	XPPerMobLevel   int32
	KillingBlowMult float32
	WindowTimeoutMs int64
}

var (
	killXPScalingMu     sync.RWMutex
	killXPScalingConfig = defaultKillXPScalingConfig()
	masteryKillMu       sync.RWMutex
	masteryKillConfig   = defaultMasteryKillScalingConfig()
)

func defaultKillXPScalingConfig() KillXPScalingConfig {
	return KillXPScalingConfig{
		BaseXPPerNPCLevel:    25,
		LevelDiffCoefficient: 0.1,
		MultiplierMin:        0.1,
		MultiplierMax:        1.5,
	}
}

func defaultMasteryKillScalingConfig() MasteryKillScalingConfig {
	return MasteryKillScalingConfig{
		XPPerMobLevel:   10,
		KillingBlowMult: 1.5,
		WindowTimeoutMs: 10000,
	}
}

func LoadAndCacheKillXPScalingConfig(cfg KillXPScalingConfig) {
	if cfg.MultiplierMin < 0 {
		cfg.MultiplierMin = 0
	}
	if cfg.MultiplierMax < cfg.MultiplierMin {
		cfg.MultiplierMax = cfg.MultiplierMin
	}
	if cfg.BaseXPPerNPCLevel < 1 {
		cfg.BaseXPPerNPCLevel = 1
	}
	killXPScalingMu.Lock()
	killXPScalingConfig = cfg
	killXPScalingMu.Unlock()
}

func GetKillXPScalingConfig() KillXPScalingConfig {
	killXPScalingMu.RLock()
	defer killXPScalingMu.RUnlock()
	return killXPScalingConfig
}

func LoadAndCacheMasteryKillScalingConfig(cfg MasteryKillScalingConfig) {
	if cfg.XPPerMobLevel < 1 {
		cfg.XPPerMobLevel = 1
	}
	if cfg.KillingBlowMult < 1.0 {
		cfg.KillingBlowMult = 1.0
	}
	if cfg.WindowTimeoutMs < 1000 {
		cfg.WindowTimeoutMs = 1000
	}
	masteryKillMu.Lock()
	masteryKillConfig = cfg
	masteryKillMu.Unlock()
}

func GetMasteryKillScalingConfig() MasteryKillScalingConfig {
	masteryKillMu.RLock()
	defer masteryKillMu.RUnlock()
	return masteryKillConfig
}

func ComputeKillXPMultiplier(playerLevel, mobLevel int) float32 {
	cfg := GetKillXPScalingConfig()
	diff := float32(mobLevel - playerLevel)
	mult := 1.0 + diff*cfg.LevelDiffCoefficient
	if mult < cfg.MultiplierMin {
		mult = cfg.MultiplierMin
	}
	if mult > cfg.MultiplierMax {
		mult = cfg.MultiplierMax
	}
	return mult
}
