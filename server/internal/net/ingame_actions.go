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

func (c *ClientConn) handleDistributeStatPoint(ctx context.Context, payload []byte) error {
	if c == nil || c.actor == nil || c.server == nil || c.server.db == nil || c.account == nil {
		return nil
	}
	r := NewReader(payload)
	statID, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	amount, err := r.ReadUint8()
	if err != nil || amount == 0 || amount > 200 {
		return nil
	}

	c.actor.Mu.Lock()
	available := c.actor.UnspentStatPoints
	if available < int32(amount) {
		c.actor.Mu.Unlock()
		log.Printf("distribute-stat REJECTED: not enough points user=%s requested=%d available=%d",
			c.account.Username, amount, available)
		return nil
	}

	newPrimary := c.actor.Primary
	switch statID {
	case 0:
		newPrimary.STR += int32(amount)
	case 1:
		newPrimary.DEX += int32(amount)
	case 2:
		newPrimary.INT += int32(amount)
	case 3:
		newPrimary.WIS += int32(amount)
	case 4:
		newPrimary.PER += int32(amount)
	default:
		c.actor.Mu.Unlock()
		log.Printf("distribute-stat REJECTED: invalid stat_id=%d user=%s", statID, c.account.Username)
		return nil
	}
	c.actor.UnspentStatPoints -= int32(amount)
	newUnspent := c.actor.UnspentStatPoints
	c.actor.Mu.Unlock()

	c.actor.SetPrimaryStats(newPrimary)
	c.recomputeStatsWithItemBonuses(ctx)
	c.actor.Mu.Lock()
	if c.actor.Health > c.actor.HealthMax {
		c.actor.Health = c.actor.HealthMax
	}
	if c.actor.Energy > c.actor.EnergyMax {
		c.actor.Energy = c.actor.EnergyMax
	}
	if c.actor.Stamina > c.actor.StaminaMax {
		c.actor.Stamina = c.actor.StaminaMax
	}
	c.actor.Mu.Unlock()

	bg := context.Background()
	if err := c.server.db.UpdateCharacterPrimaryStats(bg, c.actor.CharacterID, newPrimary, newUnspent); err != nil {
		log.Printf("distribute-stat: db error user=%s: %v", c.account.Username, err)
	}

	log.Printf("distribute-stat: user=%s stat=%d amount=%d new STR=%d DEX=%d INT=%d WIS=%d PER=%d unspent=%d",
		c.account.Username, statID, amount,
		newPrimary.STR, newPrimary.DEX, newPrimary.INT, newPrimary.WIS, newPrimary.PER, newUnspent)
	return nil
}

func (c *ClientConn) handleRespec(ctx context.Context, payload []byte) error {
	if c == nil || c.actor == nil || c.server == nil || c.server.db == nil || c.account == nil {
		return nil
	}
	r := NewReader(payload)
	confirm, err := r.ReadUint8()
	if err != nil || confirm != 1 {
		return nil
	}

	cfg := world.GetCachedCharProgressionConfig()

	c.actor.Mu.Lock()
	level := int(c.actor.Level)
	isFree := level <= cfg.RespecFreeUntilLevel

	if !isFree {
		if c.actor.Gold < int64(cfg.RespecCostGold) {
			gold := c.actor.Gold
			c.actor.Mu.Unlock()
			log.Printf("respec REJECTED: not enough gold user=%s gold=%d need=%d",
				c.account.Username, gold, cfg.RespecCostGold)
			return nil
		}
		c.actor.Gold -= int64(cfg.RespecCostGold)
	}

	initial := int32(cfg.InitialStatValue)
	if initial < 1 {
		initial = 1
	}
	pointsBack := (c.actor.Primary.STR - initial) +
		(c.actor.Primary.DEX - initial) +
		(c.actor.Primary.INT - initial) +
		(c.actor.Primary.WIS - initial) +
		(c.actor.Primary.PER - initial)
	if pointsBack < 0 {
		pointsBack = 0
	}

	newPrimary := world.PrimaryStats{STR: initial, DEX: initial, INT: initial, WIS: initial, PER: initial}
	c.actor.UnspentStatPoints += pointsBack
	if isFree {
		c.actor.FreeRespecsUsed += 1
	}
	newUnspent := c.actor.UnspentStatPoints
	newFreeUsed := c.actor.FreeRespecsUsed
	goldRemaining := c.actor.Gold
	c.actor.Mu.Unlock()

	c.actor.SetPrimaryStats(newPrimary)
	c.recomputeStatsWithItemBonuses(ctx)
	c.actor.Mu.Lock()
	if c.actor.Health > c.actor.HealthMax {
		c.actor.Health = c.actor.HealthMax
	}
	if c.actor.Energy > c.actor.EnergyMax {
		c.actor.Energy = c.actor.EnergyMax
	}
	if c.actor.Stamina > c.actor.StaminaMax {
		c.actor.Stamina = c.actor.StaminaMax
	}
	c.actor.Mu.Unlock()

	bg := context.Background()
	if err := c.server.db.UpdateCharacterPrimaryStats(bg, c.actor.CharacterID, newPrimary, newUnspent); err != nil {
		log.Printf("respec: db error user=%s: %v", c.account.Username, err)
	}
	if !isFree {
		if err := c.server.db.UpdateCharacterGold(bg, c.actor.CharacterID, goldRemaining); err != nil {
			log.Printf("respec: persist gold error user=%s: %v", c.account.Username, err)
		}
		c.sendGoldUpdate()
	} else {
		if err := c.server.db.UpdateCharacterFreeRespecsUsed(bg, c.actor.CharacterID, newFreeUsed); err != nil {
			log.Printf("respec: persist free_respecs_used error user=%s: %v", c.account.Username, err)
		}
	}

	log.Printf("respec: user=%s free=%t points_returned=%d new_unspent=%d gold_left=%d",
		c.account.Username, isFree, pointsBack, newUnspent, goldRemaining)
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
