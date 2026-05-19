package net

import (
	"fmt"
)

const (
	pSkillStateVersion1 uint8 = 1
	pSkillStateVersion2 uint8 = 2
)

// PSkillStatePayload is the server->client payload for packet 129.
type PSkillStatePayload struct {
	Version        uint8
	HasKit         bool
	KitID          uint32
	KitKey         string
	KitDisplayName string
	Abilities      []PSkillStateAbility
}

// PSkillStateAbility is one slot in the kit.
type PSkillStateAbility struct {
	SlotIndex           uint8
	AbilityID           uint32
	AbilityName         string
	CooldownMs          uint32
	CooldownRemainingMs uint32
	MasteryLevel        uint8
	MasteryXP           uint32
	MasteryXPForNext    uint32
	MasteryMaxLevel     uint8
}

// EncodePSkillState serializes the payload to wire format.
func EncodePSkillState(p PSkillStatePayload) ([]byte, error) {
	if p.Version != pSkillStateVersion1 && p.Version != pSkillStateVersion2 {
		return nil, fmt.Errorf("PSkillState: unsupported version %d", p.Version)
	}
	if len(p.Abilities) > 255 {
		return nil, fmt.Errorf("PSkillState: abilities count %d exceeds 255", len(p.Abilities))
	}

	kitKey := p.KitKey
	kitDisplayName := p.KitDisplayName
	kitID := p.KitID
	if !p.HasKit {
		kitID = 0
		kitKey = ""
		kitDisplayName = ""
	}

	var w Writer
	w.WriteUint8(p.Version)
	if p.HasKit {
		w.WriteUint8(1)
	} else {
		w.WriteUint8(0)
	}
	w.WriteUint32(kitID)

	if len(kitKey) > 65535 {
		return nil, fmt.Errorf("PSkillState: kit_key too long (%d > 65535)", len(kitKey))
	}
	if len(kitDisplayName) > 65535 {
		return nil, fmt.Errorf("PSkillState: kit_display_name too long (%d > 65535)", len(kitDisplayName))
	}
	w.WriteString(kitKey)
	w.WriteString(kitDisplayName)

	w.WriteUint8(uint8(len(p.Abilities)))
	for i, a := range p.Abilities {
		if len(a.AbilityName) > 65535 {
			return nil, fmt.Errorf("PSkillState: abilities[%d].ability_name too long (%d > 65535)", i, len(a.AbilityName))
		}
		w.WriteUint8(a.SlotIndex)
		w.WriteUint32(a.AbilityID)
		w.WriteString(a.AbilityName)
		w.WriteUint32(a.CooldownMs)
		w.WriteUint32(a.CooldownRemainingMs)
		if p.Version == pSkillStateVersion2 {
			w.WriteUint8(a.MasteryLevel)
			w.WriteUint32(a.MasteryXP)
			w.WriteUint32(a.MasteryXPForNext)
			w.WriteUint8(a.MasteryMaxLevel)
		}
	}
	return w.Bytes(), nil
}

// DecodePSkillState parses a payload from wire format.
func DecodePSkillState(buf []byte) (PSkillStatePayload, error) {
	var out PSkillStatePayload
	r := NewReader(buf)

	version, err := r.ReadUint8()
	if err != nil {
		return out, fmt.Errorf("PSkillState: read version: %w", err)
	}
	if version != pSkillStateVersion1 && version != pSkillStateVersion2 {
		return out, fmt.Errorf("PSkillState: unsupported version %d", version)
	}
	out.Version = version

	hasKitByte, err := r.ReadUint8()
	if err != nil {
		return out, fmt.Errorf("PSkillState: read has_kit: %w", err)
	}
	if hasKitByte > 1 {
		return out, fmt.Errorf("PSkillState: invalid has_kit value %d", hasKitByte)
	}
	out.HasKit = hasKitByte == 1
	out.KitID, err = r.ReadUint32()
	if err != nil {
		return out, fmt.Errorf("PSkillState: read kit_id: %w", err)
	}

	out.KitKey, err = r.ReadString()
	if err != nil {
		return out, fmt.Errorf("PSkillState: read kit_key: %w", err)
	}
	out.KitDisplayName, err = r.ReadString()
	if err != nil {
		return out, fmt.Errorf("PSkillState: read kit_display_name: %w", err)
	}

	abilityCount, err := r.ReadUint8()
	if err != nil {
		return out, fmt.Errorf("PSkillState: read ability_count: %w", err)
	}
	out.Abilities = make([]PSkillStateAbility, 0, int(abilityCount))
	for i := 0; i < int(abilityCount); i++ {
		var a PSkillStateAbility
		a.SlotIndex, err = r.ReadUint8()
		if err != nil {
			return out, fmt.Errorf("PSkillState: read abilities[%d].slot_index: %w", i, err)
		}
		a.AbilityID, err = r.ReadUint32()
		if err != nil {
			return out, fmt.Errorf("PSkillState: read abilities[%d].ability_id: %w", i, err)
		}
		a.AbilityName, err = r.ReadString()
		if err != nil {
			return out, fmt.Errorf("PSkillState: read abilities[%d].ability_name: %w", i, err)
		}
		a.CooldownMs, err = r.ReadUint32()
		if err != nil {
			return out, fmt.Errorf("PSkillState: read abilities[%d].cooldown_ms: %w", i, err)
		}
		a.CooldownRemainingMs, err = r.ReadUint32()
		if err != nil {
			return out, fmt.Errorf("PSkillState: read abilities[%d].cooldown_remaining_ms: %w", i, err)
		}
		if version == pSkillStateVersion2 {
			a.MasteryLevel, err = r.ReadUint8()
			if err != nil {
				return out, fmt.Errorf("PSkillState: read abilities[%d].mastery_level: %w", i, err)
			}
			a.MasteryXP, err = r.ReadUint32()
			if err != nil {
				return out, fmt.Errorf("PSkillState: read abilities[%d].mastery_xp: %w", i, err)
			}
			a.MasteryXPForNext, err = r.ReadUint32()
			if err != nil {
				return out, fmt.Errorf("PSkillState: read abilities[%d].mastery_xp_for_next: %w", i, err)
			}
			a.MasteryMaxLevel, err = r.ReadUint8()
			if err != nil {
				return out, fmt.Errorf("PSkillState: read abilities[%d].mastery_max_level: %w", i, err)
			}
		}
		out.Abilities = append(out.Abilities, a)
	}
	if r.r.Len() != 0 {
		return out, fmt.Errorf("PSkillState: trailing bytes: %d", r.r.Len())
	}
	return out, nil
}
