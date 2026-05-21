package net

import (
	"context"
	"fmt"
	"log"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
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
	log.Printf("ingame-action: combat user=%s rid=%d action=%d target_rid=%d received",
		c.account.Username, c.actor.RuntimeID, action, targetRID)
	return nil
}

// handleSkillLoadoutAction processes PSkillLoadoutAction (C->S).
//
// Contract v1:
//
//	action(u8) + kit_id(u32) + slot(u8) + ability_id(u32)
//
// Skill rules and persistence are implemented in the skill runtime module.
func (c *ClientConn) handleSkillLoadoutAction(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	action, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	kitID, err := r.ReadUint32()
	if err != nil {
		return nil
	}
	slot, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	abilityID, err := r.ReadUint32()
	if err != nil {
		return nil
	}
	if c.actor == nil || c.actor.IsDead() {
		return nil
	}

	charID := c.actor.CharacterID
	if c.server == nil || c.server.db == nil || charID == "" {
		return nil
	}

	switch action {
	case protocol.SkillLoadoutActionSetSlot:
		if slot >= 16 {
			log.Printf("skill-loadout: char=%s rejected: slot %d out of bounds", charID, slot)
			return nil
		}
		if err := c.handleLoadoutSetSlot(ctx, charID, int(kitID), int(slot), int(abilityID)); err != nil {
			log.Printf("skill-loadout: char=%s rejected: action=SET_SLOT kit=%d slot=%d ability=%d err=%v", charID, kitID, slot, abilityID, err)
			return nil
		}
	case protocol.SkillLoadoutActionClearSlot:
		if slot >= 16 {
			log.Printf("skill-loadout: char=%s rejected: slot %d out of bounds", charID, slot)
			return nil
		}
		if err := c.handleLoadoutClearSlot(ctx, charID, int(kitID), int(slot)); err != nil {
			log.Printf("skill-loadout: char=%s rejected: action=CLEAR_SLOT kit=%d slot=%d err=%v", charID, kitID, slot, err)
			return nil
		}
	case protocol.SkillLoadoutActionClearKit:
		if err := c.handleLoadoutClearKit(ctx, charID, int(kitID)); err != nil {
			log.Printf("skill-loadout: char=%s rejected: action=CLEAR_KIT kit=%d err=%v", charID, kitID, err)
			return nil
		}
	default:
		log.Printf("skill-loadout: char=%s rejected: unknown action=%d", charID, action)
		return nil
	}

	c.sendSkillSnapshots(ctx)
	log.Printf("skill-loadout: char=%s action=%d kit=%d slot=%d ability=%d ok",
		charID, action, kitID, slot, abilityID)
	return nil
}

// handleCastSkillSlot processes PCastSkillSlot (C->S).
//
// Contract v1:
//
//	version(u8) + slot_index(u8) + target_rid(u32)
//
// Server resolves ability_id from the authoritative active loadout snapshot
// (ResolveActivePlayerKit) and starts cast through cast_intent runtime.
func (c *ClientConn) handleCastSkillSlot(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	version, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	slotIndex, err := r.ReadUint8()
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
	if version != 1 {
		log.Printf("skill-cast: char=%s rejected: unsupported version=%d", c.actor.CharacterID, version)
		return nil
	}
	if slotIndex >= 16 {
		log.Printf("skill-cast: char=%s rejected: slot_index=%d out of bounds", c.actor.CharacterID, slotIndex)
		return nil
	}
	if targetRID == 0 {
		log.Printf("skill-cast: char=%s rejected: target_rid=0", c.actor.CharacterID)
		return nil
	}
	if c.server == nil || c.server.db == nil {
		return nil
	}
	charID := c.actor.CharacterID
	if charID == "" {
		return nil
	}

	resolution, err := c.server.db.ResolveActivePlayerKit(ctx, charID)
	if err != nil {
		log.Printf("skill-cast: char=%s resolve active kit failed: %v", charID, err)
		return nil
	}
	if !resolution.HasKit {
		log.Printf("skill-cast: char=%s rejected: no active kit", charID)
		return nil
	}

	abilityID := 0
	for _, ab := range resolution.Abilities {
		if ab.SlotIndex == int(slotIndex) {
			abilityID = ab.AbilityID
			break
		}
	}
	if abilityID <= 0 {
		log.Printf("skill-cast: char=%s rejected: slot_index=%d has no ability", charID, slotIndex)
		return nil
	}

	reasonTag := "player_hotbar"
	actionOverride := ""
	clientTraceID := fmt.Sprintf("hotbar_slot_%d", slotIndex)
	if c.server.scripting != nil {
		area, ok := c.server.world.GetArea(c.actor.AreaName)
		if ok {
			var target *world.Actor
			target, _ = area.GetActor(targetRID)
			advice := c.server.scripting.DispatchPlayerBeforeCastIntent(
				area,
				c.actor,
				target,
				abilityID,
				reasonTag,
			)
			if advice.Cancel {
				log.Printf("skill-cast: char=%s cancelled by script slot=%d ability=%d target=%d",
					charID, slotIndex, abilityID, targetRID)
				return nil
			}
			if advice.ReasonTag != "" {
				reasonTag = advice.ReasonTag
			}
			actionOverride = advice.ActionOverride
			if advice.ClientTraceID != "" {
				clientTraceID = advice.ClientTraceID
			}
		}
	}

	started, reason := world.TryStartPlayerCastByRID(c.server.world, world.CastIntent{
		CasterRID:      c.actor.RuntimeID,
		TargetRID:      targetRID,
		AbilityID:      abilityID,
		ActionOverride: actionOverride,
		ReasonTag:      reasonTag,
		ClientTraceID:  clientTraceID,
	})
	if !started {
		log.Printf("skill-cast: char=%s rejected: slot=%d ability=%d target=%d reason=%s",
			charID, slotIndex, abilityID, targetRID, reason)
		return nil
	}
	c.sendSkillState(ctx)

	// Provoke defensive/aggressive NPC target on successful cast start.
	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if ok {
		target, _ := area.GetActor(targetRID)
		if target != nil && target.IsNPC {
			target.Mu.Lock()
			if target.AIMode == world.AIWait &&
				(target.Aggressiveness == 1 || target.Aggressiveness == 2) {
				target.AIMode = world.AIChase
				target.AITarget = c.actor
			}
			target.Mu.Unlock()
		}
	}

	log.Printf("skill-cast: char=%s slot=%d ability=%d target=%d started",
		charID, slotIndex, abilityID, targetRID)
	return nil
}

func (c *ClientConn) validateLoadoutKit(ctx context.Context, kitID int) error {
	if kitID <= 0 {
		return fmt.Errorf("kit_id must be > 0")
	}
	kit, err := c.server.db.GetWeaponKitByID(ctx, kitID)
	if err != nil {
		return err
	}
	if kit == nil || !kit.Enabled {
		return fmt.Errorf("kit %d not found or disabled", kitID)
	}
	return nil
}

func (c *ClientConn) handleLoadoutSetSlot(ctx context.Context, charID string, kitID, slot, abilityID int) error {
	if err := c.validateLoadoutKit(ctx, kitID); err != nil {
		return err
	}
	if abilityID <= 0 {
		return fmt.Errorf("ability_id must be > 0")
	}

	inPool, err := c.server.db.IsAbilityInKitPool(ctx, kitID, abilityID)
	if err != nil {
		return err
	}
	if !inPool {
		return fmt.Errorf("ability %d is not in enabled pool for kit %d", abilityID, kitID)
	}

	current, err := c.server.db.ListLoadoutForCharKit(ctx, charID, kitID)
	if err != nil {
		return err
	}

	next := make([]*db.CharacterSkillLoadout, 0, len(current)+1)
	for _, entry := range current {
		if entry.AbilityID == abilityID && entry.SlotIndex != slot {
			return fmt.Errorf("ability %d already used in slot %d", abilityID, entry.SlotIndex)
		}
		if entry.SlotIndex == slot {
			continue
		}
		next = append(next, &db.CharacterSkillLoadout{
			CharacterID: charID,
			KitID:       kitID,
			SlotIndex:   entry.SlotIndex,
			AbilityID:   entry.AbilityID,
		})
	}
	next = append(next, &db.CharacterSkillLoadout{
		CharacterID: charID,
		KitID:       kitID,
		SlotIndex:   slot,
		AbilityID:   abilityID,
	})

	return c.server.db.SetLoadoutForCharKit(ctx, charID, kitID, next)
}

func (c *ClientConn) handleLoadoutClearSlot(ctx context.Context, charID string, kitID, slot int) error {
	if err := c.validateLoadoutKit(ctx, kitID); err != nil {
		return err
	}

	current, err := c.server.db.ListLoadoutForCharKit(ctx, charID, kitID)
	if err != nil {
		return err
	}

	next := make([]*db.CharacterSkillLoadout, 0, len(current))
	for _, entry := range current {
		if entry.SlotIndex == slot {
			continue
		}
		next = append(next, &db.CharacterSkillLoadout{
			CharacterID: charID,
			KitID:       kitID,
			SlotIndex:   entry.SlotIndex,
			AbilityID:   entry.AbilityID,
		})
	}

	return c.server.db.SetLoadoutForCharKit(ctx, charID, kitID, next)
}

func (c *ClientConn) handleLoadoutClearKit(ctx context.Context, charID string, kitID int) error {
	if err := c.validateLoadoutKit(ctx, kitID); err != nil {
		return err
	}
	return c.server.db.ClearLoadoutForCharKit(ctx, charID, kitID)
}
