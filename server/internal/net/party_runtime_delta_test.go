package net

import (
	"testing"

	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

type decodedPartyUpdate struct {
	mode              uint8
	partyID           uint32
	leaderRID         uint32
	members           []partyMemberWire
	upserts           []partyMemberWire
	removals          []uint32
	pendingInviteFrom string
	noticeCode        uint8
	notice            string
}

func newPartyUpdateTestServer() *Server {
	return &Server{
		party:              newPartyManager(5),
		clientsByCharacter: make(map[string]*ClientConn),
		clientsByName:      make(map[string]*ClientConn),
		clientsByRuntimeID: make(map[uint32]*ClientConn),
	}
}

func newPartyUpdateTestClient(
	s *Server,
	rid uint32,
	characterID, name, areaName string,
	x, z float32,
) *ClientConn {
	c := &ClientConn{
		server: s,
		state:  StateInGame,
		actor:  world.NewActor(),
	}
	c.actor.RuntimeID = rid
	c.actor.CharacterID = characterID
	c.actor.Name = name
	c.actor.AreaName = areaName
	c.actor.X = x
	c.actor.Z = z
	c.actor.Level = 10
	c.actor.Health = 120
	c.actor.HealthMax = 150
	s.registerInGameClient(c)
	return c
}

func readPartyMemberWire(t *testing.T, r *Reader) partyMemberWire {
	t.Helper()
	rid, err := r.ReadUint32()
	if err != nil {
		t.Fatalf("read member rid: %v", err)
	}
	name, err := r.ReadString()
	if err != nil {
		t.Fatalf("read member name: %v", err)
	}
	level, err := r.ReadUint16()
	if err != nil {
		t.Fatalf("read member level: %v", err)
	}
	hp, err := r.ReadUint16()
	if err != nil {
		t.Fatalf("read member hp: %v", err)
	}
	hpMax, err := r.ReadUint16()
	if err != nil {
		t.Fatalf("read member hp max: %v", err)
	}
	online, err := r.ReadBool()
	if err != nil {
		t.Fatalf("read member online: %v", err)
	}
	return partyMemberWire{
		RuntimeID: rid,
		Name:      name,
		Level:     level,
		HP:        hp,
		HPMax:     hpMax,
		Online:    online,
	}
}

func decodePartyUpdatePacket(t *testing.T, framed []byte) decodedPartyUpdate {
	t.Helper()
	frame := NewReader(framed)
	packetType, err := frame.ReadUint16()
	if err != nil {
		t.Fatalf("read packet type: %v", err)
	}
	if packetType != protocol.PPartyUpdate {
		t.Fatalf("unexpected packet type: got=%d want=%d", packetType, protocol.PPartyUpdate)
	}
	payloadLen, err := frame.ReadUint32()
	if err != nil {
		t.Fatalf("read payload length: %v", err)
	}
	payload := make([]byte, payloadLen)
	for i := uint32(0); i < payloadLen; i++ {
		b, readErr := frame.ReadUint8()
		if readErr != nil {
			t.Fatalf("read payload byte[%d]: %v", i, readErr)
		}
		payload[i] = b
	}

	var out decodedPartyUpdate
	r := NewReader(payload)
	out.mode, err = r.ReadUint8()
	if err != nil {
		t.Fatalf("read mode: %v", err)
	}
	out.partyID, err = r.ReadUint32()
	if err != nil {
		t.Fatalf("read party id: %v", err)
	}
	out.leaderRID, err = r.ReadUint32()
	if err != nil {
		t.Fatalf("read leader rid: %v", err)
	}

	switch out.mode {
	case partyUpdateModeSnapshot:
		memberCount, readErr := r.ReadUint8()
		if readErr != nil {
			t.Fatalf("read snapshot member count: %v", readErr)
		}
		out.members = make([]partyMemberWire, 0, memberCount)
		for i := uint8(0); i < memberCount; i++ {
			out.members = append(out.members, readPartyMemberWire(t, r))
		}
	case partyUpdateModeDelta:
		upsertCount, readErr := r.ReadUint8()
		if readErr != nil {
			t.Fatalf("read delta upsert count: %v", readErr)
		}
		out.upserts = make([]partyMemberWire, 0, upsertCount)
		for i := uint8(0); i < upsertCount; i++ {
			out.upserts = append(out.upserts, readPartyMemberWire(t, r))
		}

		removeCount, removeErr := r.ReadUint8()
		if removeErr != nil {
			t.Fatalf("read delta remove count: %v", removeErr)
		}
		out.removals = make([]uint32, 0, removeCount)
		for i := uint8(0); i < removeCount; i++ {
			removeRID, ridErr := r.ReadUint32()
			if ridErr != nil {
				t.Fatalf("read delta remove rid: %v", ridErr)
			}
			out.removals = append(out.removals, removeRID)
		}
	default:
		t.Fatalf("unexpected party update mode: %d", out.mode)
	}

	out.pendingInviteFrom, err = r.ReadString()
	if err != nil {
		t.Fatalf("read pending invite: %v", err)
	}
	out.noticeCode, err = r.ReadUint8()
	if err != nil {
		t.Fatalf("read notice code: %v", err)
	}
	out.notice, err = r.ReadString()
	if err != nil {
		t.Fatalf("read notice text: %v", err)
	}
	return out
}

func readOnePartyUpdatePacket(t *testing.T, actor *world.Actor) decodedPartyUpdate {
	t.Helper()
	select {
	case framed := <-actor.SendCh:
		return decodePartyUpdatePacket(t, framed)
	default:
		t.Fatalf("expected party update packet, but channel was empty")
		return decodedPartyUpdate{}
	}
}

func expectNoPartyUpdatePacket(t *testing.T, actor *world.Actor) {
	t.Helper()
	select {
	case framed := <-actor.SendCh:
		t.Fatalf("expected no party update packet, got %d bytes", len(framed))
	default:
	}
}

func TestPartyUpdateSnapshotThenDeltaForMemberHPChange(t *testing.T) {
	s := newPartyUpdateTestServer()
	alice := newPartyUpdateTestClient(s, 101, "char_a", "Alice", "Starter Zone", 0, 0)
	bob := newPartyUpdateTestClient(s, 202, "char_b", "Bob", "Starter Zone", 6, 0)

	partyID, err := s.party.invite("char_a", "Alice", "char_b")
	if err != nil {
		t.Fatalf("invite failed: %v", err)
	}
	if _, err := s.party.accept("char_b"); err != nil {
		t.Fatalf("accept failed: %v", err)
	}

	if err := s.sendPartySnapshotToClient(alice, protocol.PartyNoticeNone, ""); err != nil {
		t.Fatalf("send snapshot failed: %v", err)
	}
	first := readOnePartyUpdatePacket(t, alice.actor)
	if first.mode != partyUpdateModeSnapshot {
		t.Fatalf("expected initial snapshot mode, got=%d", first.mode)
	}
	if first.partyID != partyID {
		t.Fatalf("unexpected party id in snapshot: got=%d want=%d", first.partyID, partyID)
	}
	if len(first.members) != 2 {
		t.Fatalf("expected 2 members in snapshot, got=%d", len(first.members))
	}

	if err := s.sendPartySnapshotToClient(alice, protocol.PartyNoticeNone, ""); err != nil {
		t.Fatalf("send no-op update failed: %v", err)
	}
	expectNoPartyUpdatePacket(t, alice.actor)

	bob.actor.Mu.Lock()
	bob.actor.Health = 77
	bob.actor.Mu.Unlock()

	if err := s.sendPartySnapshotToClient(alice, protocol.PartyNoticeNone, ""); err != nil {
		t.Fatalf("send delta update failed: %v", err)
	}
	second := readOnePartyUpdatePacket(t, alice.actor)
	if second.mode != partyUpdateModeDelta {
		t.Fatalf("expected delta mode after HP change, got=%d", second.mode)
	}
	if len(second.upserts) != 1 {
		t.Fatalf("expected exactly 1 upsert after single-member HP change, got=%d", len(second.upserts))
	}
	if second.upserts[0].RuntimeID != bob.actor.RuntimeID {
		t.Fatalf("unexpected upsert target rid: got=%d want=%d", second.upserts[0].RuntimeID, bob.actor.RuntimeID)
	}
	if second.upserts[0].HP != 77 {
		t.Fatalf("unexpected upsert HP: got=%d want=77", second.upserts[0].HP)
	}
	if len(second.removals) != 0 {
		t.Fatalf("expected no removals on HP-only delta, got=%d", len(second.removals))
	}
}

func TestPartyUpdateDeltaForMemberRemovalAndSnapshotReset(t *testing.T) {
	s := newPartyUpdateTestServer()
	alice := newPartyUpdateTestClient(s, 101, "char_a", "Alice", "Starter Zone", 0, 0)
	bob := newPartyUpdateTestClient(s, 202, "char_b", "Bob", "Starter Zone", 4, 0)

	if _, err := s.party.invite("char_a", "Alice", "char_b"); err != nil {
		t.Fatalf("invite failed: %v", err)
	}
	if _, err := s.party.accept("char_b"); err != nil {
		t.Fatalf("accept failed: %v", err)
	}
	if err := s.sendPartySnapshotToClient(alice, protocol.PartyNoticeNone, ""); err != nil {
		t.Fatalf("send initial snapshot failed: %v", err)
	}
	_ = readOnePartyUpdatePacket(t, alice.actor)

	if _, _, _, err := s.party.leave("char_b"); err != nil {
		t.Fatalf("bob leave failed: %v", err)
	}
	if err := s.sendPartySnapshotToClient(alice, protocol.PartyNoticeLeft, "Bob left the party."); err != nil {
		t.Fatalf("send removal delta failed: %v", err)
	}
	delta := readOnePartyUpdatePacket(t, alice.actor)
	if delta.mode != partyUpdateModeDelta {
		t.Fatalf("expected delta mode when member leaves same party, got=%d", delta.mode)
	}
	if len(delta.removals) != 1 || delta.removals[0] != bob.actor.RuntimeID {
		t.Fatalf("unexpected removals: got=%v want=[%d]", delta.removals, bob.actor.RuntimeID)
	}
	if delta.noticeCode != protocol.PartyNoticeLeft {
		t.Fatalf("unexpected notice code on leave delta: got=%d want=%d", delta.noticeCode, protocol.PartyNoticeLeft)
	}

	if _, _, _, err := s.party.leave("char_a"); err != nil {
		t.Fatalf("alice leave failed: %v", err)
	}
	if err := s.sendPartySnapshotToClient(alice, protocol.PartyNoticeLeft, "You left the party."); err != nil {
		t.Fatalf("send reset snapshot failed: %v", err)
	}
	reset := readOnePartyUpdatePacket(t, alice.actor)
	if reset.mode != partyUpdateModeSnapshot {
		t.Fatalf("expected snapshot mode when party id resets to zero, got=%d", reset.mode)
	}
	if reset.partyID != 0 {
		t.Fatalf("expected party id 0 after final leave, got=%d", reset.partyID)
	}
	if len(reset.members) != 0 {
		t.Fatalf("expected zero members after full leave, got=%d", len(reset.members))
	}
}
