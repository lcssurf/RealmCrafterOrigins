package world

import "testing"

func TestComputeDerivedStats_AllZero(t *testing.T) {
	primary := PrimaryStats{0, 0, 0, 0, 0}
	d := ComputeDerivedStats(primary, 1, 0, 0)

	wantHP := healthBase + healthPerLevel
	if d.HealthMax != wantHP {
		t.Errorf("HealthMax: got %d, want %d", d.HealthMax, wantHP)
	}
	if d.MeleeCritValue != critPerLevel {
		t.Errorf("MeleeCritValue: got %d, want %d", d.MeleeCritValue, critPerLevel)
	}
	if d.MeleeDefenseValue != defPerLevel {
		t.Errorf("MeleeDefenseValue (no armor): got %d, want %d", d.MeleeDefenseValue, defPerLevel)
	}
}

func TestComputeDerivedStats_CritCap(t *testing.T) {
	primary := PrimaryStats{0, 1000, 0, 0, 0}
	d := ComputeDerivedStats(primary, 1, 0, 0)
	if d.MeleeCritValue != 8001 {
		t.Fatalf("MeleeCritValue with 1000 DEX at level 1: got %d, want 8001", d.MeleeCritValue)
	}

	pct := ValueToPercent(d.MeleeCritValue, critValueCap, critValueSoftcap)
	if pct < 0.50 || pct > critValueCap+0.01 {
		t.Errorf("high DEX crit pct: got %.3f, want close to cap %.3f", pct, critValueCap)
	}
}

func TestComputeDerivedStats_Level60Reference(t *testing.T) {
	primary := PrimaryStats{180, 180, 180, 180, 180}
	d := ComputeDerivedStats(primary, 60, 50, 100)

	// HP baseline reference:
	// 100 + (180 * 8) + (60 * 15) = 2440
	if d.HealthMax != 2440 {
		t.Errorf("level 60 HP wrong: got %d, want 2440", d.HealthMax)
	}
	if d.MeleeCritValue != 1500 {
		t.Errorf("level 60 MeleeCritValue: got %d, want 1500", d.MeleeCritValue)
	}
	pct := ValueToPercent(d.MeleeCritValue, critValueCap, critValueSoftcap)
	if pct < 0.30 || pct > 0.40 {
		t.Errorf("level 60 MeleeCrit pct: got %.3f, want 0.30..0.40", pct)
	}
}

func TestValueToPercent_Curve(t *testing.T) {
	cases := []struct {
		name    string
		value   int32
		cap     float32
		softcap int32
		wantMin float32
		wantMax float32
	}{
		{name: "low", value: 100, cap: 0.70, softcap: 1500, wantMin: 0.04, wantMax: 0.05},
		{name: "mid", value: 1500, cap: 0.70, softcap: 1500, wantMin: 0.30, wantMax: 0.40},
		{name: "high", value: 5000, cap: 0.70, softcap: 1500, wantMin: 0.50, wantMax: 0.60},
		{name: "zero", value: 0, cap: 0.70, softcap: 1500, wantMin: 0.0, wantMax: 0.0},
		{name: "negative", value: -100, cap: 0.70, softcap: 1500, wantMin: 0.0, wantMax: 0.0},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			pct := ValueToPercent(tc.value, tc.cap, tc.softcap)
			if pct < tc.wantMin || pct > tc.wantMax {
				t.Errorf("ValueToPercent(%d, %.2f, %d)=%.3f; want %.3f..%.3f",
					tc.value, tc.cap, tc.softcap, pct, tc.wantMin, tc.wantMax)
			}
		})
	}
}

func TestValueToPercent_Capped(t *testing.T) {
	const hugeValue int32 = 2_000_000_000
	pct := ValueToPercent(hugeValue, critValueCap, critValueSoftcap)
	if pct > critValueCap {
		t.Errorf("ValueToPercent should never exceed cap: got %.6f, cap %.6f", pct, critValueCap)
	}
	if pct < 0.69 {
		t.Errorf("very large value should approach cap: got %.6f, want >= 0.69", pct)
	}
}

func TestValueToPercent_Monotonic(t *testing.T) {
	values := []int32{0, 100, 500, 1500, 3000, 6000}
	var prev float32
	for i, v := range values {
		pct := ValueToPercent(v, critValueCap, critValueSoftcap)
		if i > 0 && pct < prev {
			t.Errorf("curve must be monotonic: value=%d pct=%.6f < prev=%.6f", v, pct, prev)
		}
		prev = pct
	}
}
