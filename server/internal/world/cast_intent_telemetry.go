package world

import (
	"log"
	"sort"
	"strings"
	"sync"
)

const castIntentTelemetryLogIntervalMs int64 = 30000

// CastIntentReasonTelemetry is one aggregated row for a reason_tag.
type CastIntentReasonTelemetry struct {
	ReasonTag     string
	Attempts      int64
	Started       int64
	Rejected      int64
	RejectReasons map[string]int64
}

type castIntentReasonTelemetryState struct {
	Attempts      int64
	Started       int64
	Rejected      int64
	RejectReasons map[string]int64
}

var (
	castIntentTelemetryMu        sync.Mutex
	castIntentTelemetryByReason  = map[string]*castIntentReasonTelemetryState{}
	castIntentTelemetryLastLogMs int64
)

func normalizeCastIntentReasonTag(tag string, casterKind int) string {
	tag = strings.TrimSpace(strings.ToLower(tag))
	if tag != "" {
		return tag
	}
	switch casterKind {
	case castIntentCasterNPC:
		return "npc_ai"
	case castIntentCasterPlayer:
		return "player_input"
	default:
		return "unknown"
	}
}

func normalizeCastIntentRejectReason(reason string) string {
	reason = strings.TrimSpace(strings.ToLower(reason))
	if reason == "" {
		return "unknown"
	}
	return reason
}

func recordCastIntentTelemetry(intent CastIntent, casterKind int, started bool, rejectReason string, now int64) {
	key := normalizeCastIntentReasonTag(intent.ReasonTag, casterKind)
	reject := normalizeCastIntentRejectReason(rejectReason)

	castIntentTelemetryMu.Lock()
	row := castIntentTelemetryByReason[key]
	if row == nil {
		row = &castIntentReasonTelemetryState{
			RejectReasons: make(map[string]int64),
		}
		castIntentTelemetryByReason[key] = row
	}
	row.Attempts++
	if started {
		row.Started++
	} else {
		row.Rejected++
		row.RejectReasons[reject]++
	}
	shouldLog := false
	if castIntentTelemetryLastLogMs == 0 || now-castIntentTelemetryLastLogMs >= castIntentTelemetryLogIntervalMs {
		castIntentTelemetryLastLogMs = now
		shouldLog = true
	}
	castIntentTelemetryMu.Unlock()

	if shouldLog {
		logCastIntentTelemetrySnapshot()
	}
}

func logCastIntentTelemetrySnapshot() {
	snapshot := getCastIntentTelemetrySnapshot()
	if len(snapshot) == 0 {
		return
	}
	sort.Slice(snapshot, func(i, j int) bool {
		if snapshot[i].Attempts != snapshot[j].Attempts {
			return snapshot[i].Attempts > snapshot[j].Attempts
		}
		return snapshot[i].ReasonTag < snapshot[j].ReasonTag
	})
	for _, row := range snapshot {
		log.Printf("telemetry: cast_intent reason_tag=%s attempts=%d started=%d rejected=%d rejects=%v",
			row.ReasonTag, row.Attempts, row.Started, row.Rejected, row.RejectReasons)
	}
}

func getCastIntentTelemetrySnapshot() []CastIntentReasonTelemetry {
	castIntentTelemetryMu.Lock()
	defer castIntentTelemetryMu.Unlock()
	return getCastIntentTelemetrySnapshotLocked()
}

func getCastIntentTelemetrySnapshotLocked() []CastIntentReasonTelemetry {
	out := make([]CastIntentReasonTelemetry, 0, len(castIntentTelemetryByReason))
	for key, row := range castIntentTelemetryByReason {
		rejects := make(map[string]int64, len(row.RejectReasons))
		for reason, count := range row.RejectReasons {
			rejects[reason] = count
		}
		out = append(out, CastIntentReasonTelemetry{
			ReasonTag:     key,
			Attempts:      row.Attempts,
			Started:       row.Started,
			Rejected:      row.Rejected,
			RejectReasons: rejects,
		})
	}
	return out
}

func resetCastIntentTelemetry() {
	castIntentTelemetryMu.Lock()
	castIntentTelemetryByReason = map[string]*castIntentReasonTelemetryState{}
	castIntentTelemetryLastLogMs = 0
	castIntentTelemetryMu.Unlock()
}
