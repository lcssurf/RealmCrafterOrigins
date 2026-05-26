package net

import (
	"context"
	"log"
	"strings"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/world"
)

// handleNPCKilledMasteryDistribute receives world kill-window callbacks.
// XP persistence runs asynchronously so combat runtime remains non-blocking.
func (s *Server) handleNPCKilledMasteryDistribute(charID string, abilityID uint32, xp int64, isKillingBlow bool) {
	if s == nil || s.db == nil || strings.TrimSpace(charID) == "" || abilityID == 0 || xp <= 0 {
		return
	}
	gain := int(xp)
	maxInt := int(^uint(0) >> 1)
	if xp > int64(maxInt) {
		gain = maxInt
	}
	if gain <= 0 {
		return
	}
	go s.grantPlayerSkillXPAmount(charID, int(abilityID), gain, isKillingBlow)
}

func (s *Server) grantPlayerSkillXPAmount(charID string, abilityID int, xpAmount int, isKillingBlow bool) {
	if s == nil || s.db == nil || strings.TrimSpace(charID) == "" || abilityID <= 0 || xpAmount <= 0 {
		return
	}

	ability, ok := world.GetAbilityTemplateByID(abilityID)
	if !ok {
		log.Printf("mastery: ability template missing for char=%s ability=%d", charID, abilityID)
		return
	}

	ctx := context.Background()

	progress, err := s.db.GetCharacterSkillProgress(ctx, charID, abilityID)
	if err != nil {
		log.Printf("mastery: get progress failed char=%s ability=%d err=%v", charID, abilityID, err)
		return
	}
	if progress == nil {
		progress = &db.CharacterSkillProgress{
			CharacterID: charID,
			AbilityID:   abilityID,
			XP:          0,
			Level:       1,
		}
	}

	oldLevel := progress.Level
	if oldLevel < 1 {
		oldLevel = 1
	}
	progress.XP, progress.Level, _ = db.ProcessMasteryXPCumulative(
		progress.XP,
		progress.Level,
		xpAmount,
		&ability,
	)

	if err := s.db.UpsertCharacterSkillProgress(ctx, progress); err != nil {
		log.Printf("mastery: upsert failed char=%s ability=%d err=%v", charID, abilityID, err)
		return
	}

	var client *ClientConn
	if progress.Level > oldLevel {
		client = s.findClientByCharacterID(charID)
		if client != nil && client.actor != nil {
			client.actor.Mu.Lock()
			if client.actor.SkillLevels == nil {
				client.actor.SkillLevels = make(map[int]int)
			}
			client.actor.SkillLevels[abilityID] = progress.Level
			client.actor.Mu.Unlock()
		}
		log.Printf("mastery: char=%s ability=%d LEVEL UP %d->%d xp=%d gain=%d killing_blow=%t",
			charID, abilityID, oldLevel, progress.Level, progress.XP, xpAmount, isKillingBlow)
	} else {
		log.Printf("mastery: char=%s ability=%d xp=%d level=%d gain=%d killing_blow=%t",
			charID, abilityID, progress.XP, progress.Level, xpAmount, isKillingBlow)
	}

	if client == nil {
		client = s.findClientByCharacterID(charID)
	}
	if client != nil {
		client.sendSkillState(ctx)
	}
}
