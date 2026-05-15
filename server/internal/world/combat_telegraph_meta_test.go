package world

import (
	"strings"
	"testing"
)

func TestBuildSpecialWindupMetaTextIncludesCoreFields(t *testing.T) {
	ability := AbilityTemplate{
		ID:                 9901,
		TelegraphType:      "ring_close",
		TelegraphRadius:    3.25,
		TelegraphColorRGBA: "1,0.45,0.15,0.75",
	}
	got := buildSpecialWindupMetaText(ability, "boss_phase_2", "trace-abc")
	if !strings.HasPrefix(got, "meta:") {
		t.Fatalf("meta prefix mismatch: %q", got)
	}
	for _, want := range []string{
		"ability=9901",
		"reason=boss_phase_2",
		"radius=3.25",
		"color=1,0.45,0.15,0.75",
		"style=ring_close",
		"window_ms=",
		"trace=trace-abc",
	} {
		if !strings.Contains(got, want) {
			t.Fatalf("meta field missing %q in %q", want, got)
		}
	}
}
