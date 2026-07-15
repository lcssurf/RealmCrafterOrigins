package net

import "fmt"

const pKitPoolVersion1 uint8 = 1
const pKitPoolVersion2 uint8 = 2

// PKitPoolPayload is the server->client payload for packet 132.
type PKitPoolPayload struct {
	Version        uint8
	KitID          uint32
	KitKey         string
	KitDisplayName string
	Abilities      []PKitPoolAbility
}

// PKitPoolAbility is one ability available in the active kit pool.
type PKitPoolAbility struct {
	AbilityID   uint32
	AbilityName string
	CooldownMs  uint32
	// IconPath: UI icon shown on the hotbar (migrateV53, version 2+). "" =
	// client keeps drawing the placeholder rect.
	IconPath string
}

// EncodePKitPool serializes the payload to wire format.
func EncodePKitPool(p PKitPoolPayload) ([]byte, error) {
	if p.Version != pKitPoolVersion1 && p.Version != pKitPoolVersion2 {
		return nil, fmt.Errorf("PKitPool: unsupported version %d", p.Version)
	}
	if len(p.Abilities) > 255 {
		return nil, fmt.Errorf("PKitPool: abilities count %d exceeds 255", len(p.Abilities))
	}

	kitID := p.KitID
	kitKey := p.KitKey
	kitDisplayName := p.KitDisplayName
	abilities := p.Abilities
	if kitID == 0 {
		kitKey = ""
		kitDisplayName = ""
		abilities = nil
	}

	if len(kitKey) > 65535 {
		return nil, fmt.Errorf("PKitPool: kit_key too long (%d > 65535)", len(kitKey))
	}
	if len(kitDisplayName) > 65535 {
		return nil, fmt.Errorf("PKitPool: kit_display_name too long (%d > 65535)", len(kitDisplayName))
	}

	var w Writer
	w.WriteUint8(p.Version)
	w.WriteUint32(kitID)
	w.WriteString(kitKey)
	w.WriteString(kitDisplayName)
	w.WriteUint8(uint8(len(abilities)))
	for i, a := range abilities {
		if len(a.AbilityName) > 65535 {
			return nil, fmt.Errorf("PKitPool: abilities[%d].ability_name too long (%d > 65535)", i, len(a.AbilityName))
		}
		w.WriteUint32(a.AbilityID)
		w.WriteString(a.AbilityName)
		w.WriteUint32(a.CooldownMs)
		if p.Version >= pKitPoolVersion2 {
			w.WriteString(a.IconPath)
		}
	}
	return w.Bytes(), nil
}

// DecodePKitPool parses a payload from wire format.
func DecodePKitPool(buf []byte) (PKitPoolPayload, error) {
	var out PKitPoolPayload
	r := NewReader(buf)

	version, err := r.ReadUint8()
	if err != nil {
		return out, fmt.Errorf("PKitPool: read version: %w", err)
	}
	if version != pKitPoolVersion1 && version != pKitPoolVersion2 {
		return out, fmt.Errorf("PKitPool: unsupported version %d", version)
	}
	out.Version = version

	out.KitID, err = r.ReadUint32()
	if err != nil {
		return out, fmt.Errorf("PKitPool: read kit_id: %w", err)
	}
	out.KitKey, err = r.ReadString()
	if err != nil {
		return out, fmt.Errorf("PKitPool: read kit_key: %w", err)
	}
	out.KitDisplayName, err = r.ReadString()
	if err != nil {
		return out, fmt.Errorf("PKitPool: read kit_display_name: %w", err)
	}

	abilityCount, err := r.ReadUint8()
	if err != nil {
		return out, fmt.Errorf("PKitPool: read ability_count: %w", err)
	}
	out.Abilities = make([]PKitPoolAbility, 0, int(abilityCount))
	for i := 0; i < int(abilityCount); i++ {
		var a PKitPoolAbility
		a.AbilityID, err = r.ReadUint32()
		if err != nil {
			return out, fmt.Errorf("PKitPool: read abilities[%d].ability_id: %w", i, err)
		}
		a.AbilityName, err = r.ReadString()
		if err != nil {
			return out, fmt.Errorf("PKitPool: read abilities[%d].ability_name: %w", i, err)
		}
		a.CooldownMs, err = r.ReadUint32()
		if err != nil {
			return out, fmt.Errorf("PKitPool: read abilities[%d].cooldown_ms: %w", i, err)
		}
		if version >= pKitPoolVersion2 {
			a.IconPath, err = r.ReadString()
			if err != nil {
				return out, fmt.Errorf("PKitPool: read abilities[%d].icon_path: %w", i, err)
			}
		}
		out.Abilities = append(out.Abilities, a)
	}
	if r.r.Len() != 0 {
		return out, fmt.Errorf("PKitPool: trailing bytes: %d", r.r.Len())
	}
	return out, nil
}

