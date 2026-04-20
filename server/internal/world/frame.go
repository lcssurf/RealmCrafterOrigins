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

// newActorPayload builds a PNewActor payload for any actor.
func newActorPayload(a *Actor) []byte {
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
	return p
}
