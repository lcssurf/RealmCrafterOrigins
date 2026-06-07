package net

import (
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

func (s *Server) handleAbilityFXBroadcast(
	area *world.Area,
	casterRID, targetRID uint32,
	abilityID uint32,
	vfxPath, sfxPath string,
	posX, posY, posZ float32,
	magnitude float32,
	phase string,
) {
	if s == nil || area == nil {
		return
	}
	if vfxPath != "" {
		s.broadcastEmitterRich(area, vfxPath, casterRID, targetRID, abilityID, magnitude, posX, posY, posZ, phase)
	}
	if sfxPath != "" {
		s.broadcastSoundRich(area, sfxPath, casterRID, targetRID, abilityID, magnitude, phase)
	}
}

func (s *Server) broadcastEmitterRich(
	area *world.Area,
	vfxPath string,
	casterRID, targetRID, abilityID uint32,
	magnitude, posX, posY, posZ float32,
	phase string,
) {
	if s == nil || area == nil {
		return
	}

	var w Writer
	// Legacy layout for backward compatibility.
	w.WriteUint8(0)
	w.WriteFloat32(posX)
	w.WriteFloat32(posY)
	w.WriteFloat32(posZ)
	w.WriteUint16(2000)

	// Rich context fields (appended).
	w.WriteString(vfxPath)
	w.WriteUint32(casterRID)
	w.WriteUint32(targetRID)
	w.WriteUint32(abilityID)
	w.WriteFloat32(magnitude)
	w.WriteString(phase)

	area.BroadcastAll(buildFramedPacket(protocol.PCreateEmitter, w.Bytes()))
}

func (s *Server) broadcastSoundRich(
	area *world.Area,
	sfxPath string,
	casterRID, targetRID, abilityID uint32,
	magnitude float32,
	phase string,
) {
	if s == nil || area == nil {
		return
	}

	var w Writer
	// Legacy layout for backward compatibility.
	w.WriteUint8(0)
	w.WriteUint8(255)

	// Rich context fields (appended).
	w.WriteString(sfxPath)
	w.WriteUint32(casterRID)
	w.WriteUint32(targetRID)
	w.WriteUint32(abilityID)
	w.WriteFloat32(magnitude)
	w.WriteString(phase)

	area.BroadcastAll(buildFramedPacket(protocol.PSound, w.Bytes()))
}
