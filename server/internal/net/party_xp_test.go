package net

import (
	"testing"

	"realm-crafter/server/internal/world"
)

func newPartyXPTestServer() *Server {
	return &Server{
		party:              newPartyManager(5),
		clientsByCharacter: make(map[string]*ClientConn),
		clientsByName:      make(map[string]*ClientConn),
		clientsByRuntimeID: make(map[uint32]*ClientConn),
	}
}

func newPartyXPTestClient(
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
	c.actor.Health = 100
	c.actor.HealthMax = 100
	s.registerInGameClient(c)
	return c
}

func TestCollectXPRecipientsSoloFallback(t *testing.T) {
	s := newPartyXPTestServer()
	killer := newPartyXPTestClient(s, 1, "char_a", "Alice", "Starter Zone", 0, 0)

	recipients := killer.collectXPRecipients(0, 0)
	if len(recipients) != 1 || recipients[0] != killer {
		t.Fatalf("solo recipients mismatch: got=%d", len(recipients))
	}
}

func TestCollectXPRecipientsPartyByProximity(t *testing.T) {
	s := newPartyXPTestServer()
	killer := newPartyXPTestClient(s, 1, "char_a", "Alice", "Starter Zone", 0, 0)
	near := newPartyXPTestClient(s, 2, "char_b", "Bob", "Starter Zone", 30, 0)
	far := newPartyXPTestClient(s, 3, "char_c", "Carol", "Starter Zone", 300, 0)
	otherArea := newPartyXPTestClient(s, 4, "char_d", "Dan", "Forest", 10, 0)

	if _, err := s.party.invite("char_a", "Alice", "char_b"); err != nil {
		t.Fatalf("invite near failed: %v", err)
	}
	if _, err := s.party.accept("char_b"); err != nil {
		t.Fatalf("accept near failed: %v", err)
	}
	if _, err := s.party.invite("char_a", "Alice", "char_c"); err != nil {
		t.Fatalf("invite far failed: %v", err)
	}
	if _, err := s.party.accept("char_c"); err != nil {
		t.Fatalf("accept far failed: %v", err)
	}
	if _, err := s.party.invite("char_a", "Alice", "char_d"); err != nil {
		t.Fatalf("invite other-area failed: %v", err)
	}
	if _, err := s.party.accept("char_d"); err != nil {
		t.Fatalf("accept other-area failed: %v", err)
	}

	recipients := killer.collectXPRecipients(0, 0)
	got := map[string]bool{}
	for _, recipient := range recipients {
		if recipient == nil || recipient.actor == nil {
			continue
		}
		got[recipient.actor.CharacterID] = true
	}
	if !got["char_a"] || !got["char_b"] {
		t.Fatalf("expected killer and near member to receive XP: %+v", got)
	}
	if got["char_c"] {
		t.Fatalf("far member should not receive XP share")
	}
	if got["char_d"] {
		t.Fatalf("other-area member should not receive XP share")
	}

	_ = near // keep explicit reader intent in test setup
	_ = far
	_ = otherArea
}

func TestDistributePartyXPGainRemainderToKiller(t *testing.T) {
	s := newPartyXPTestServer()
	killer := newPartyXPTestClient(s, 1, "char_a", "Alice", "Starter Zone", 0, 0)
	near := newPartyXPTestClient(s, 2, "char_b", "Bob", "Starter Zone", 0, 0)

	parts := []*ClientConn{killer, near}
	out := distributePartyXPGain(51, parts, "char_a")
	if out[killer] != 26 {
		t.Fatalf("killer share mismatch: got=%d want=26", out[killer])
	}
	if out[near] != 25 {
		t.Fatalf("near member share mismatch: got=%d want=25", out[near])
	}
}
