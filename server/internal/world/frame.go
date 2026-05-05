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

// appendAnimBindings writes the animation section shared by AppearanceBytes and
// NewActorPayload:
//
//	binding_count u16,
//	for each binding:
//	  action str, source_path str, start_frame i32, end_frame i32, fps f32,
//	  loop u8, speed f32, blend_in f32, return_to str, priority u8,
//	  event_count u16,
//	  for each event: frame i32, event_type str, payload str
func appendAnimBindings(p *pb, anims []AnimBinding) {
	na := len(anims)
	if na > 255 {
		na = 255
	}
	p.u16(uint16(na))
	for i := 0; i < na; i++ {
		b := &anims[i]
		p.str(b.Action)
		p.str(b.SourcePath)
		p.str(b.ClipOverride)
		p.i32(b.StartFrame)
		p.i32(b.EndFrame)
		p.f32(b.FPS)
		if b.Loop {
			p.u8(1)
		} else {
			p.u8(0)
		}
		p.f32(b.Speed)
		p.f32(b.BlendIn)
		p.str(b.ReturnTo)
		p.u8(b.Priority)
		ne := len(b.Events)
		if ne > 65535 {
			ne = 65535
		}
		p.u16(uint16(ne))
		for j := 0; j < ne; j++ {
			ev := &b.Events[j]
			p.i32(ev.Frame)
			p.str(ev.EventType)
			p.str(ev.Payload)
		}
	}
}

// AppearanceBytes serialises only the appearance section (num_meshes … binding_count)
// using the same layout as the tail of NewActorPayload. The result can be
// appended to any packet that needs to carry appearance data (e.g. PStartGame).
func AppearanceBytes(app *Appearance) []byte {
	var p pb
	if app == nil {
		p.u8(0)   // num_meshes
		p.u16(0)  // binding_count
		p.f32(0)  // yaw_offset
		p.f32(0)  // y_offset
		return p
	}
	n := len(app.Meshes)
	if n > 255 {
		n = 255
	}
	p.u8(uint8(n))
	for i := 0; i < n; i++ {
		m := &app.Meshes[i]
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
		nm := len(m.MaterialMap)
		if nm > 255 {
			nm = 255
		}
		p.u8(uint8(nm))
		for j := 0; j < nm; j++ {
			am := &m.MaterialMap[j]
			p.str(am.AiName)
			p.str(am.AlbedoPath)
			p.str(am.NormalPath)
			p.str(am.ORMPath)
			p.f32(am.AlbedoR)
			p.f32(am.AlbedoG)
			p.f32(am.AlbedoB)
			p.f32(am.Roughness)
			p.f32(am.Metallic)
		}
	}
	appendAnimBindings(&p, app.Anims)
	p.f32(app.YawOffset)
	p.f32(app.YOffset)
	return p
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
//     num_ai_mats u8, for each: ai_name str + PBR paths + factors,
//   binding_count u16,
//   for each binding:
//     action str, source_path str, start_frame i32, end_frame i32, fps f32,
//     loop u8, speed f32, blend_in f32, return_to str, priority u8,
//     event_count u16, for each event: frame i32, event_type str, payload str
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
		p.u8(0)   // num_meshes
		p.u16(0)  // binding_count
		p.f32(0)  // yaw_offset
		p.f32(0)  // y_offset
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

		// Per-aiMaterial mapping — paint multi-material meshes correctly
		// (e.g. Substance-named blinn slots). Capped at 255 entries.
		nm := len(m.MaterialMap)
		if nm > 255 {
			nm = 255
		}
		p.u8(uint8(nm))
		for j := 0; j < nm; j++ {
			am := &m.MaterialMap[j]
			p.str(am.AiName)
			p.str(am.AlbedoPath)
			p.str(am.NormalPath)
			p.str(am.ORMPath)
			p.f32(am.AlbedoR)
			p.f32(am.AlbedoG)
			p.f32(am.AlbedoB)
			p.f32(am.Roughness)
			p.f32(am.Metallic)
		}
	}

	appendAnimBindings(&p, a.Appearance.Anims)
	p.f32(a.Appearance.YawOffset)
	p.f32(a.Appearance.YOffset)
	return p
}

// WorldObjectsPayload encodes a slice of static world objects for PWorldObjects.
// Format: count(u16) + for each: model_path(str)+scale(f32)+x(f32)+y(f32)+z(f32)+yaw(f32)
func WorldObjectsPayload(objects []WorldObject) []byte {
	var p pb
	n := len(objects)
	if n > 0xFFFF {
		n = 0xFFFF
	}
	p.u16(uint16(n))
	for i := 0; i < n; i++ {
		o := &objects[i]
		p.str(o.ModelPath)
		p.f32(o.Scale)
		p.f32(o.X)
		p.f32(o.Y)
		p.f32(o.Z)
		p.f32(o.Yaw)
	}
	return []byte(p)
}
