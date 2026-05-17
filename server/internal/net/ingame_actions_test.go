package net

import (
	"context"
	"testing"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

func TestQuestActionPayloadCodec(t *testing.T) {
	var w Writer
	w.WriteUint8(protocol.QuestActionAccept)
	w.WriteUint32(42)

	r := NewReader(w.Bytes())
	action, err := r.ReadUint8()
	if err != nil {
		t.Fatalf("read action: %v", err)
	}
	questID, err := r.ReadUint32()
	if err != nil {
		t.Fatalf("read quest id: %v", err)
	}

	if action != protocol.QuestActionAccept {
		t.Fatalf("unexpected action: got=%d want=%d", action, protocol.QuestActionAccept)
	}
	if questID != 42 {
		t.Fatalf("unexpected quest id: got=%d want=42", questID)
	}
}

func TestPartyActionPayloadCodec(t *testing.T) {
	var w Writer
	w.WriteUint8(protocol.PartyActionInvite)
	w.WriteString("TargetPlayer")

	r := NewReader(w.Bytes())
	action, err := r.ReadUint8()
	if err != nil {
		t.Fatalf("read action: %v", err)
	}
	target, err := r.ReadString()
	if err != nil {
		t.Fatalf("read target: %v", err)
	}

	if action != protocol.PartyActionInvite {
		t.Fatalf("unexpected action: got=%d want=%d", action, protocol.PartyActionInvite)
	}
	if target != "TargetPlayer" {
		t.Fatalf("unexpected target: got=%q want=%q", target, "TargetPlayer")
	}
}

func TestCombatActionPayloadCodec(t *testing.T) {
	var w Writer
	w.WriteUint8(protocol.CombatActionInterrupt)
	w.WriteUint32(77)

	r := NewReader(w.Bytes())
	action, err := r.ReadUint8()
	if err != nil {
		t.Fatalf("read action: %v", err)
	}
	targetRID, err := r.ReadUint32()
	if err != nil {
		t.Fatalf("read target rid: %v", err)
	}

	if action != protocol.CombatActionInterrupt {
		t.Fatalf("unexpected action: got=%d want=%d", action, protocol.CombatActionInterrupt)
	}
	if targetRID != 77 {
		t.Fatalf("unexpected target rid: got=%d want=77", targetRID)
	}
}

func TestSkillLoadoutActionPayloadCodec(t *testing.T) {
	var w Writer
	w.WriteUint8(protocol.SkillLoadoutActionSetSlot)
	w.WriteUint32(5001)
	w.WriteUint8(3)
	w.WriteUint32(9001)

	r := NewReader(w.Bytes())
	action, err := r.ReadUint8()
	if err != nil {
		t.Fatalf("read action: %v", err)
	}
	kitID, err := r.ReadUint32()
	if err != nil {
		t.Fatalf("read kit id: %v", err)
	}
	slot, err := r.ReadUint8()
	if err != nil {
		t.Fatalf("read slot: %v", err)
	}
	abilityID, err := r.ReadUint32()
	if err != nil {
		t.Fatalf("read ability id: %v", err)
	}

	if action != protocol.SkillLoadoutActionSetSlot {
		t.Fatalf("unexpected action: got=%d want=%d", action, protocol.SkillLoadoutActionSetSlot)
	}
	if kitID != 5001 || slot != 3 || abilityID != 9001 {
		t.Fatalf("unexpected payload values: kit=%d slot=%d ability=%d", kitID, slot, abilityID)
	}
}

func TestInGameActionHandlersIgnoreMalformedPayload(t *testing.T) {
	c := &ClientConn{
		account: &db.Account{Username: "tester"},
		actor:   world.NewActor(),
	}

	if err := c.handleQuestAction(context.Background(), []byte{1}); err != nil {
		t.Fatalf("quest malformed should be ignored, got err=%v", err)
	}
	if err := c.handlePartyAction(context.Background(), []byte{1}); err != nil {
		t.Fatalf("party malformed should be ignored, got err=%v", err)
	}
	if err := c.handleCombatAction(context.Background(), []byte{1}); err != nil {
		t.Fatalf("combat malformed should be ignored, got err=%v", err)
	}
	if err := c.handleSkillLoadoutAction(context.Background(), []byte{1, 0}); err != nil {
		t.Fatalf("skill-loadout malformed should be ignored, got err=%v", err)
	}
}

func TestInGameActionHandlersIgnoreWhenActorMissing(t *testing.T) {
	c := &ClientConn{
		account: &db.Account{Username: "tester"},
		actor:   nil,
	}

	var w Writer
	w.WriteUint8(protocol.QuestActionAccept)
	w.WriteUint32(7)
	if err := c.handleQuestAction(context.Background(), w.Bytes()); err != nil {
		t.Fatalf("quest action without actor should be ignored, got err=%v", err)
	}
}
