package net

import (
	"testing"
	"time"

	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

func newCombatTestServer() *Server {
	return &Server{
		world:              world.New(),
		party:              newPartyManager(5),
		clientsByCharacter: make(map[string]*ClientConn),
		clientsByName:      make(map[string]*ClientConn),
		clientsByRuntimeID: make(map[uint32]*ClientConn),
	}
}

func newCombatTestClient(
	s *Server,
	rid uint32,
	characterID, name string,
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
	c.actor.AreaName = "Starter Zone"
	c.actor.X = x
	c.actor.Z = z
	c.actor.Health = 100
	c.actor.HealthMax = 100
	c.actor.Energy = 100
	c.actor.EnergyMax = 100
	c.actor.Stamina = 100
	c.actor.StaminaMax = 100
	s.world.GetOrCreateArea("Starter Zone").AddActor(c.actor)
	s.registerInGameClient(c)
	return c
}

func decodeFramedPacket(t *testing.T, framed []byte) (uint16, []byte) {
	t.Helper()
	r := NewReader(framed)
	packetType, err := r.ReadUint16()
	if err != nil {
		t.Fatalf("read packet type: %v", err)
	}
	payloadLen, err := r.ReadUint32()
	if err != nil {
		t.Fatalf("read payload len: %v", err)
	}
	payload := make([]byte, payloadLen)
	for i := uint32(0); i < payloadLen; i++ {
		b, readErr := r.ReadUint8()
		if readErr != nil {
			t.Fatalf("read payload byte[%d]: %v", i, readErr)
		}
		payload[i] = b
	}
	return packetType, payload
}

func findCombatEventCode(t *testing.T, actor *world.Actor) uint8 {
	t.Helper()
	deadline := time.After(250 * time.Millisecond)
	for {
		select {
		case framed := <-actor.SendCh:
			packetType, payload := decodeFramedPacket(t, framed)
			if packetType != protocol.PCombatEvent {
				continue
			}
			r := NewReader(payload)
			code, err := r.ReadUint8()
			if err != nil {
				t.Fatalf("read combat event code: %v", err)
			}
			return code
		case <-deadline:
			t.Fatalf("timeout waiting for combat event packet")
			return 0
		}
	}
}

func TestProcessCombatActionDodgeConsumesSPAndSetsWindow(t *testing.T) {
	s := newCombatTestServer()
	c := newCombatTestClient(s, 101, "char_a", "Alice", 0, 0)
	c.actor.Stamina = 30

	before := time.Now().UnixMilli()
	c.processCombatAction(protocol.CombatActionDodge, 0)

	c.actor.Mu.Lock()
	gotStamina := c.actor.Stamina
	gotWindow := c.actor.DodgeUntil
	c.actor.Mu.Unlock()

	if gotStamina != 12 {
		t.Fatalf("unexpected dodge SP after cost: got=%d want=12", gotStamina)
	}
	if gotWindow < before+combatDodgeIFrameMs-5 {
		t.Fatalf("dodge window not set correctly: got=%d want>=%d", gotWindow, before+combatDodgeIFrameMs-5)
	}
	if code := findCombatEventCode(t, c.actor); code != protocol.CombatEventDodgeStarted {
		t.Fatalf("unexpected combat event code: got=%d want=%d", code, protocol.CombatEventDodgeStarted)
	}
}

func TestProcessCombatActionInterruptBreaksGuard(t *testing.T) {
	s := newCombatTestServer()
	attacker := newCombatTestClient(s, 101, "char_a", "Alice", 0, 0)
	target := newCombatTestClient(s, 202, "char_b", "Bob", 1.5, 0)

	target.actor.Mu.Lock()
	target.actor.Guarding = true
	target.actor.GuardUntil = time.Now().UnixMilli() + 1000
	target.actor.Mu.Unlock()

	attacker.processCombatAction(protocol.CombatActionInterrupt, target.actor.RuntimeID)

	target.actor.Mu.Lock()
	stillGuarding := target.actor.Guarding
	target.actor.Mu.Unlock()
	if stillGuarding {
		t.Fatalf("target should stop guarding after successful interrupt")
	}

	if code := findCombatEventCode(t, attacker.actor); code != protocol.CombatEventInterruptSuccess {
		t.Fatalf("unexpected combat event code: got=%d want=%d", code, protocol.CombatEventInterruptSuccess)
	}
}
