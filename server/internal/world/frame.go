package world

import (
	"encoding/binary"
	"math"
)

// buildFrame creates a fully framed packet: [u16 type LE][u32 payloadLen LE][payload].
func buildFrame(pktType uint16, payload []byte) []byte {
	buf := make([]byte, 6+len(payload))
	binary.LittleEndian.PutUint16(buf[0:2], pktType)
	binary.LittleEndian.PutUint32(buf[2:6], uint32(len(payload)))
	copy(buf[6:], payload)
	return buf
}

// pb is a tiny packet payload builder.
type pb []byte

func (b *pb) u8(v uint8)  { *b = append(*b, v) }
func (b *pb) u16(v uint16) {
	var tmp [2]byte
	binary.LittleEndian.PutUint16(tmp[:], v)
	*b = append(*b, tmp[:]...)
}
func (b *pb) i16(v int16) { b.u16(uint16(v)) }
func (b *pb) i32(v int32) { b.u32(uint32(v)) }
func (b *pb) u32(v uint32) {
	var tmp [4]byte
	binary.LittleEndian.PutUint32(tmp[:], v)
	*b = append(*b, tmp[:]...)
}
func (b *pb) f32(v float32) { b.u32(math.Float32bits(v)) }
func (b *pb) str(s string) {
	b.u16(uint16(len(s)))
	*b = append(*b, []byte(s)...)
}

// NewActorPayload builds a PNewActor payload for any actor. Exported so the
// `net` package can use it when a new client enters an area and needs to be
// informed about the existing actors. Keeping this as the single source of
// truth prevents the protocol from drifting between call sites.
//
// Payload layout:
//   rid u32, name str, race str, class str, level u16,
//   x f32, y f32, z f32, yaw f32, health i32, health_max i32, actor_type u8,
//   — appearance —
//   num_meshes u8,
//   for each mesh:
//     slot u8, model_path str, scale f32,
//     albedo str, normal str, orm str,
//     albedo_r f32, albedo_g f32, albedo_b f32, roughness f32, metallic f32,
//   num_anims u8,
//   for each anim:
//     action str, source_path str, clip_override str
//
// If Appearance is nil (or has zero meshes), num_meshes=0 and the client
// falls back to its default model for this actor.
func NewActorPayload(a *Actor) []byte {
	var p pb
	p.u32(a.RuntimeID)
	p.str(a.Name)
	p.str(a.Race)
	p.str(a.Class)
	p.u16(a.Level)
	p.f32(a.X)
	p.f32(a.Y)
	p.f32(a.Z)
	p.f32(a.Yaw)
	a.Mu.Lock()
	p.i32(a.Health)
	p.i32(a.HealthMax)
	a.Mu.Unlock()
	p.u8(a.ActorType())

	// Appearance
	if a.Appearance == nil {
		p.u8(0) // num_meshes
		p.u8(0) // num_anims
		return p
	}
	n := len(a.Appearance.Meshes)
	if n > 255 {
		n = 255
	}
	p.u8(uint8(n))
	for i := 0; i < n; i++ {
		m := &a.Appearance.Meshes[i]
		p.u8(m.Slot)
		p.str(m.ModelPath)
		if m.Scale == 0 {
			p.f32(1.0)
		} else {
			p.f32(m.Scale)
		}
		p.str(m.AlbedoPath)
		p.str(m.NormalPath)
		p.str(m.ORMPath)
		p.f32(m.AlbedoR)
		p.f32(m.AlbedoG)
		p.f32(m.AlbedoB)
		p.f32(m.Roughness)
		p.f32(m.Metallic)
	}

	na := len(a.Appearance.Anims)
	if na > 255 {
		na = 255
	}
	p.u8(uint8(na))
	for i := 0; i < na; i++ {
		b := &a.Appearance.Anims[i]
		p.str(b.Action)
		p.str(b.SourcePath)
		p.str(b.ClipOverride)
	}
	return p
}
