package net

import (
	"testing"

	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

func TestPartyInviteAcceptFlow(t *testing.T) {
	pm := newPartyManager(5)

	partyID, err := pm.invite("char_a", "Alice", "char_b")
	if err != nil {
		t.Fatalf("invite failed: %v", err)
	}
	if partyID == 0 {
		t.Fatalf("expected non-zero party id")
	}

	snapA := pm.snapshotFor("char_a")
	if snapA.PartyID != partyID {
		t.Fatalf("unexpected party id for inviter: got=%d want=%d", snapA.PartyID, partyID)
	}
	if len(snapA.MemberCharacterIDs) != 1 || snapA.MemberCharacterIDs[0] != "char_a" {
		t.Fatalf("unexpected inviter member list: %+v", snapA.MemberCharacterIDs)
	}

	snapB := pm.snapshotFor("char_b")
	if snapB.PendingInviteFrom != "Alice" {
		t.Fatalf("expected pending invite from Alice, got=%q", snapB.PendingInviteFrom)
	}

	acceptedPartyID, err := pm.accept("char_b")
	if err != nil {
		t.Fatalf("accept failed: %v", err)
	}
	if acceptedPartyID != partyID {
		t.Fatalf("unexpected party id on accept: got=%d want=%d", acceptedPartyID, partyID)
	}

	snapB = pm.snapshotFor("char_b")
	if snapB.PartyID != partyID {
		t.Fatalf("target should be in party after accept: got=%d want=%d", snapB.PartyID, partyID)
	}
	if len(snapB.MemberCharacterIDs) != 2 {
		t.Fatalf("expected 2 members after accept, got=%d", len(snapB.MemberCharacterIDs))
	}
	if snapB.PendingInviteFrom != "" {
		t.Fatalf("pending invite should be cleared after accept, got=%q", snapB.PendingInviteFrom)
	}
}

func TestPartyLeaderRulesAndTransfer(t *testing.T) {
	pm := newPartyManager(5)

	partyID, err := pm.invite("char_a", "Alice", "char_b")
	if err != nil {
		t.Fatalf("invite failed: %v", err)
	}
	if _, err := pm.accept("char_b"); err != nil {
		t.Fatalf("accept failed: %v", err)
	}

	if _, err := pm.invite("char_b", "Bob", "char_c"); err == nil {
		t.Fatalf("non-leader invite should fail")
	}

	_, members, err := pm.transferLeader("char_a", "char_b")
	if err != nil {
		t.Fatalf("transfer leader failed: %v", err)
	}
	if len(members) != 2 {
		t.Fatalf("expected 2 members after transfer, got=%d", len(members))
	}

	if _, err := pm.invite("char_b", "Bob", "char_c"); err != nil {
		t.Fatalf("new leader invite should succeed: %v", err)
	}
	if _, err := pm.accept("char_c"); err != nil {
		t.Fatalf("third member accept failed: %v", err)
	}

	snap := pm.snapshotFor("char_c")
	if snap.PartyID != partyID {
		t.Fatalf("unexpected party for char_c: got=%d want=%d", snap.PartyID, partyID)
	}
	if snap.LeaderCharacterID != "char_b" {
		t.Fatalf("expected char_b as leader, got=%q", snap.LeaderCharacterID)
	}
}

func TestPartyLeaveAndCleanup(t *testing.T) {
	pm := newPartyManager(4)

	if _, err := pm.invite("char_a", "Alice", "char_b"); err != nil {
		t.Fatalf("invite failed: %v", err)
	}
	if _, err := pm.accept("char_b"); err != nil {
		t.Fatalf("accept failed: %v", err)
	}

	partyID, remaining, clearedInvites, err := pm.leave("char_b")
	if err != nil {
		t.Fatalf("leave failed: %v", err)
	}
	if partyID == 0 {
		t.Fatalf("expected party id from leave")
	}
	if len(remaining) != 1 || remaining[0] != "char_a" {
		t.Fatalf("unexpected remaining members after leave: %+v", remaining)
	}
	if len(clearedInvites) != 0 {
		t.Fatalf("unexpected cleared invites after regular leave: %+v", clearedInvites)
	}

	if _, _, _, err := pm.leave("char_a"); err != nil {
		t.Fatalf("leader leave failed: %v", err)
	}
	snap := pm.snapshotFor("char_a")
	if snap.PartyID != 0 {
		t.Fatalf("expected no party after all leaves, got=%d", snap.PartyID)
	}
}

func TestPartyInviteAllowsTargetRegardlessOfDistance(t *testing.T) {
	s := &Server{
		party:              newPartyManager(5),
		clientsByCharacter: make(map[string]*ClientConn),
		clientsByName:      make(map[string]*ClientConn),
		clientsByRuntimeID: make(map[uint32]*ClientConn),
	}

	a := &ClientConn{server: s, state: StateInGame, actor: world.NewActor()}
	a.actor.RuntimeID = 100
	a.actor.CharacterID = "char_a"
	a.actor.Name = "Alice"
	a.actor.AreaName = "Starter Zone"
	a.actor.X = 0
	a.actor.Z = 0

	b := &ClientConn{server: s, state: StateInGame, actor: world.NewActor()}
	b.actor.RuntimeID = 200
	b.actor.CharacterID = "char_b"
	b.actor.Name = "Bob"
	b.actor.AreaName = "Starter Zone"
	b.actor.X = 10000
	b.actor.Z = 0

	s.registerInGameClient(a)
	s.registerInGameClient(b)

	a.processPartyAction(protocol.PartyActionInvite, "Bob")

	snapA := s.party.snapshotFor("char_a")
	if snapA.PartyID == 0 {
		t.Fatalf("expected inviter party to be created on invite")
	}
	snapB := s.party.snapshotFor("char_b")
	if snapB.PendingInviteFrom != "Alice" {
		t.Fatalf("expected pending invite for target, got=%q", snapB.PendingInviteFrom)
	}

	select {
	case pkt := <-a.actor.SendCh:
		frame := NewReader(pkt)
		packetType, err := frame.ReadUint16()
		if err != nil {
			t.Fatalf("read packet type: %v", err)
		}
		if packetType != protocol.PPartyUpdate {
			t.Fatalf("unexpected packet type: got=%d want=%d", packetType, protocol.PPartyUpdate)
		}
		payloadLen, err := frame.ReadUint32()
		if err != nil {
			t.Fatalf("read payload len: %v", err)
		}
		payload := make([]byte, payloadLen)
		for i := uint32(0); i < payloadLen; i++ {
			b, err := frame.ReadUint8()
			if err != nil {
				t.Fatalf("read payload byte %d: %v", i, err)
			}
			payload[i] = b
		}

		r := NewReader(payload)
		if _, err := r.ReadUint8(); err != nil { // mode
			t.Fatalf("read mode: %v", err)
		}
		if _, err := r.ReadUint32(); err != nil { // party id
			t.Fatalf("read party id: %v", err)
		}
		if _, err := r.ReadUint32(); err != nil { // leader rid
			t.Fatalf("read leader rid: %v", err)
		}
		memberCount, err := r.ReadUint8()
		if err != nil {
			t.Fatalf("read member count: %v", err)
		}
		for i := uint8(0); i < memberCount; i++ {
			if _, err := r.ReadUint32(); err != nil {
				t.Fatalf("read member rid: %v", err)
			}
			if _, err := r.ReadString(); err != nil {
				t.Fatalf("read member name: %v", err)
			}
			if _, err := r.ReadUint16(); err != nil {
				t.Fatalf("read member level: %v", err)
			}
			if _, err := r.ReadUint16(); err != nil {
				t.Fatalf("read member hp: %v", err)
			}
			if _, err := r.ReadUint16(); err != nil {
				t.Fatalf("read member hp max: %v", err)
			}
			if _, err := r.ReadBool(); err != nil {
				t.Fatalf("read member online: %v", err)
			}
		}
		if _, err := r.ReadString(); err != nil { // pending invite from
			t.Fatalf("read pending invite: %v", err)
		}
		noticeCode, err := r.ReadUint8()
		if err != nil {
			t.Fatalf("read notice code: %v", err)
		}
		if noticeCode != protocol.PartyNoticeInviteSent {
			t.Fatalf("unexpected notice code: got=%d want=%d", noticeCode, protocol.PartyNoticeInviteSent)
		}
	default:
		t.Fatalf("expected invite sent notice packet for inviter")
	}
}
