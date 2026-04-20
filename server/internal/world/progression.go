package world

// ---------------------------------------------------------------------------
// Progression tables — isolate here so they can be ported to Lua/GUE later.
// ---------------------------------------------------------------------------

const MaxLevel = 60

// xpThresholds[i] = total XP required to reach level i+2 (index 0 = level 2).
// Formula: level^2 * 100. Easy to swap for a hand-crafted table.
var xpThresholds [MaxLevel]int64

func init() {
	for i := 0; i < MaxLevel; i++ {
		lvl := int64(i + 2)
		xpThresholds[i] = (lvl - 1) * (lvl - 1) * 100
	}
}

// XPToLevel returns the total XP required to reach the given level (1-based).
// Level 1 always requires 0 XP.
func XPToLevel(level int) int64 {
	if level <= 1 {
		return 0
	}
	idx := level - 2
	if idx >= MaxLevel {
		return xpThresholds[MaxLevel-1] + int64(level)*10000
	}
	return xpThresholds[idx]
}

// XPForKill returns the XP reward for killing an NPC of the given level.
func XPForKill(npcLevel int) int64 {
	return int64(npcLevel) * 25
}

// StatsByLevel returns (healthMax, energyMax, strength) for a player at the given level.
func StatsByLevel(level int) (healthMax, energyMax, strength int32) {
	l := int32(level)
	healthMax = 100 + l*15
	energyMax = 50 + l*5
	strength = l * 3
	return
}

// ProcessXP applies an XP gain to a character and returns the updated values.
// Returns (newXP, newLevel, leveled).
func ProcessXP(currentXP int64, currentLevel int, gain int64) (newXP int64, newLevel int, leveled bool) {
	newXP = currentXP + gain
	newLevel = currentLevel
	for newLevel < MaxLevel && newXP >= XPToLevel(newLevel+1) {
		newLevel++
		leveled = true
	}
	return
}
