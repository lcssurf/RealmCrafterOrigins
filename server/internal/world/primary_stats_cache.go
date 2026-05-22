package world

import "sync"

var (
	primaryStatsMu    sync.RWMutex
	primaryStatsCache map[int]PrimaryStats
)

type PrimaryStatsRow struct {
	Level         int
	STR, DEX, INT int32
	WIS, PER      int32
}

func LoadAndCachePrimaryStatsPerLevel(rows []PrimaryStatsRow) {
	cache := make(map[int]PrimaryStats, len(rows))
	for _, row := range rows {
		cache[row.Level] = PrimaryStats{
			STR: row.STR,
			DEX: row.DEX,
			INT: row.INT,
			WIS: row.WIS,
			PER: row.PER,
		}
	}

	primaryStatsMu.Lock()
	primaryStatsCache = cache
	primaryStatsMu.Unlock()
}

func PrimaryStatsForLevel(level int) PrimaryStats {
	primaryStatsMu.RLock()
	stats, ok := primaryStatsCache[level]
	primaryStatsMu.RUnlock()
	if ok {
		return stats
	}

	if level < 1 {
		level = 1
	}
	fallback := int32(level * 3)
	return PrimaryStats{
		STR: fallback,
		DEX: fallback,
		INT: fallback,
		WIS: fallback,
		PER: fallback,
	}
}
