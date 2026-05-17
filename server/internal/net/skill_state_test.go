package net

import (
	"reflect"
	"strings"
	"testing"
)

func TestEncodeDecodePSkillState_NoKit(t *testing.T) {
	p := PSkillStatePayload{
		Version: 1,
		HasKit:  false,
	}
	buf, err := EncodePSkillState(p)
	if err != nil {
		t.Fatalf("EncodePSkillState: %v", err)
	}
	decoded, err := DecodePSkillState(buf)
	if err != nil {
		t.Fatalf("DecodePSkillState: %v", err)
	}
	if decoded.HasKit {
		t.Fatalf("decoded.HasKit = true, want false")
	}
	if decoded.KitKey != "" {
		t.Fatalf("decoded.KitKey = %q, want empty", decoded.KitKey)
	}
	if decoded.KitDisplayName != "" {
		t.Fatalf("decoded.KitDisplayName = %q, want empty", decoded.KitDisplayName)
	}
	if len(decoded.Abilities) != 0 {
		t.Fatalf("len(decoded.Abilities) = %d, want 0", len(decoded.Abilities))
	}
}

func TestEncodeDecodePSkillState_SwordKit(t *testing.T) {
	p := PSkillStatePayload{
		Version:        1,
		HasKit:         true,
		KitKey:         "sword",
		KitDisplayName: "Sword",
		Abilities: []PSkillStateAbility{
			{SlotIndex: 0, AbilityID: 1, AbilityName: "sword_slash", CooldownMs: 600, CooldownRemainingMs: 0},
			{SlotIndex: 1, AbilityID: 2, AbilityName: "sword_cleave", CooldownMs: 5000, CooldownRemainingMs: 0},
		},
	}

	buf, err := EncodePSkillState(p)
	if err != nil {
		t.Fatalf("EncodePSkillState: %v", err)
	}
	decoded, err := DecodePSkillState(buf)
	if err != nil {
		t.Fatalf("DecodePSkillState: %v", err)
	}
	if !reflect.DeepEqual(decoded, p) {
		t.Fatalf("roundtrip mismatch:\n got: %#v\nwant: %#v", decoded, p)
	}
}

func TestDecodePSkillState_UnsupportedVersion(t *testing.T) {
	var w Writer
	w.WriteUint8(99)
	w.WriteUint8(0)
	w.WriteUint8(0)
	w.WriteUint8(0)
	w.WriteUint8(0)

	if _, err := DecodePSkillState(w.Bytes()); err == nil {
		t.Fatalf("expected unsupported version error, got nil")
	}
}

func TestEncodePSkillState_StringTooLongU16(t *testing.T) {
	p := PSkillStatePayload{
		Version: 1,
		HasKit:  true,
		KitKey:  strings.Repeat("x", 65536),
	}
	if _, err := EncodePSkillState(p); err == nil {
		t.Fatalf("expected string too long error, got nil")
	}
}
