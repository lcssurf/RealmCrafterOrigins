package net

import (
	"reflect"
	"strings"
	"testing"
)

func TestEncodeDecodePSkillState_NoKit(t *testing.T) {
	p := PSkillStatePayload{
		Version: 3,
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
	if decoded.KitID != 0 {
		t.Fatalf("decoded.KitID = %d, want 0", decoded.KitID)
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
		Version:        3,
		HasKit:         true,
		KitID:          7,
		KitKey:         "sword",
		KitDisplayName: "Sword",
		Abilities: []PSkillStateAbility{
			{
				SlotIndex:           0,
				AbilityID:           1,
				AbilityName:         "sword_slash",
				CooldownMs:          600,
				CooldownRemainingMs: 0,
				MasteryLevel:        3,
				MasteryXP:           250,
				MasteryXPForNext:    300,
				MasteryMaxLevel:     10,
				Description:         "Single-target slash attack.",
			},
			{
				SlotIndex:           1,
				AbilityID:           2,
				AbilityName:         "sword_cleave",
				CooldownMs:          5000,
				CooldownRemainingMs: 0,
				MasteryLevel:        1,
				MasteryXP:           0,
				MasteryXPForNext:    100,
				MasteryMaxLevel:     10,
				Description:         "Wide cleave that hits nearby enemies.",
			},
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

func TestDecodePSkillState_V1BackwardCompatDefaultsMastery(t *testing.T) {
	p := PSkillStatePayload{
		Version:        1,
		HasKit:         true,
		KitID:          7,
		KitKey:         "sword",
		KitDisplayName: "Sword",
		Abilities: []PSkillStateAbility{
			{SlotIndex: 0, AbilityID: 1, AbilityName: "sword_slash", CooldownMs: 600, CooldownRemainingMs: 0},
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
	if len(decoded.Abilities) != 1 {
		t.Fatalf("len(decoded.Abilities) = %d, want 1", len(decoded.Abilities))
	}
	got := decoded.Abilities[0]
	if got.MasteryLevel != 0 || got.MasteryXP != 0 || got.MasteryXPForNext != 0 || got.MasteryMaxLevel != 0 {
		t.Fatalf("expected v1 mastery defaults zero, got %+v", got)
	}
	if got.Description != "" {
		t.Fatalf("expected v1 description default empty, got %q", got.Description)
	}
}

func TestDecodePSkillState_V2BackwardCompatDefaultsDescription(t *testing.T) {
	p := PSkillStatePayload{
		Version:        2,
		HasKit:         true,
		KitID:          7,
		KitKey:         "sword",
		KitDisplayName: "Sword",
		Abilities: []PSkillStateAbility{
			{
				SlotIndex:           0,
				AbilityID:           1,
				AbilityName:         "sword_slash",
				CooldownMs:          600,
				CooldownRemainingMs: 0,
				MasteryLevel:        2,
				MasteryXP:           120,
				MasteryXPForNext:    200,
				MasteryMaxLevel:     10,
			},
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
	if len(decoded.Abilities) != 1 {
		t.Fatalf("len(decoded.Abilities) = %d, want 1", len(decoded.Abilities))
	}
	if decoded.Abilities[0].Description != "" {
		t.Fatalf("expected v2 description default empty, got %q", decoded.Abilities[0].Description)
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
		Version: 2,
		HasKit:  true,
		KitKey:  strings.Repeat("x", 65536),
	}
	if _, err := EncodePSkillState(p); err == nil {
		t.Fatalf("expected string too long error, got nil")
	}
}
