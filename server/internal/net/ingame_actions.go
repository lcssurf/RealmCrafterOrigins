package net

import (
	"context"
	"log"
)

// handleQuestAction processes PQuestAction (C->S).
//
// Contract v1:
//
//	action(u8) + quest_id(u32)
//
// Current behavior: validate payload and actor context, then log a structured
// trace. Quest business rules are implemented in the quest runtime module.
func (c *ClientConn) handleQuestAction(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	action, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	questID, err := r.ReadUint32()
	if err != nil {
		return nil
	}
	if c.actor == nil || c.actor.IsDead() {
		return nil
	}
	if c.server == nil || c.server.db == nil {
		return nil
	}

	questChanged, err := c.executeQuestAction(ctx, action, int(questID))
	if err != nil {
		log.Printf("ingame-action: quest rejected user=%s rid=%d action=%d quest_id=%d err=%v",
			c.account.Username, c.actor.RuntimeID, action, questID, err)
		return nil
	}

	log.Printf("ingame-action: quest user=%s rid=%d action=%d quest_id=%d changed=%t",
		c.account.Username, c.actor.RuntimeID, action, questID, questChanged)
	return nil
}

// handlePartyAction processes PPartyAction (C->S).
//
// Contract v1:
//
//	action(u8) + target_name(str)
//
// target_name can be empty for actions that do not require a target.
func (c *ClientConn) handlePartyAction(_ context.Context, payload []byte) error {
	r := NewReader(payload)
	action, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	targetName, err := r.ReadString()
	if err != nil {
		return nil
	}
	if c.actor == nil || c.actor.IsDead() {
		return nil
	}
	c.processPartyAction(action, targetName)
	log.Printf("ingame-action: party user=%s rid=%d action=%d target=%q processed",
		c.account.Username, c.actor.RuntimeID, action, targetName)
	return nil
}

// handleCombatAction processes PCombatAction (C->S).
//
// Contract v1:
//
//	action(u8) + target_rid(u32)
//
// Combat validation/execution is implemented in the combat runtime module.
func (c *ClientConn) handleCombatAction(_ context.Context, payload []byte) error {
	r := NewReader(payload)
	action, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	targetRID, err := r.ReadUint32()
	if err != nil {
		return nil
	}
	if c.actor == nil || c.actor.IsDead() {
		return nil
	}
	c.processCombatAction(action, targetRID)
	log.Printf("ingame-action: combat user=%s rid=%d action=%d target_rid=%d processed",
		c.account.Username, c.actor.RuntimeID, action, targetRID)
	return nil
}

// handleSkillLoadoutAction processes PSkillLoadoutAction (C->S).
//
// Contract v1:
//
//	action(u8) + skill_id(u16) + slot(u8) + preset(u8)
//
// Skill rules and persistence are implemented in the skill runtime module.
func (c *ClientConn) handleSkillLoadoutAction(_ context.Context, payload []byte) error {
	r := NewReader(payload)
	action, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	skillID, err := r.ReadUint16()
	if err != nil {
		return nil
	}
	slot, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	preset, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	if c.actor == nil || c.actor.IsDead() {
		return nil
	}
	log.Printf("ingame-action: skill-loadout user=%s rid=%d action=%d skill_id=%d slot=%d preset=%d",
		c.account.Username, c.actor.RuntimeID, action, skillID, slot, preset)
	return nil
}
