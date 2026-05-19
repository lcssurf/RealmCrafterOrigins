package net

import (
	"context"
	"log"
	"strings"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/world"
)

// handleSpecialHit receives world special-hit callbacks for successful impacts.
// XP persistence runs asynchronously so the area AI tick remains non-blocking.
func (s *Server) handleSpecialHit(_ *world.Area, attacker *world.Actor, _ *world.Actor, abilityID int) {
	if s == nil || s.db == nil || attacker == nil || attacker.IsNPC || abilityID <= 0 {
		return
	}
	charID := strings.TrimSpace(attacker.CharacterID)
	if charID == "" {
		return
	}

	go s.grantPlayerSkillXP(charID, abilityID)
}

func (s *Server) grantPlayerSkillXP(charID string, abilityID int) {
	if s == nil || s.db == nil || strings.TrimSpace(charID) == "" || abilityID <= 0 {
		return
	}

	ability, ok := world.GetAbilityTemplateByID(abilityID)
	if !ok {
		log.Printf("mastery: ability template missing for char=%s ability=%d", charID, abilityID)
		return
	}
	xpPerUse := ability.MasteryXPPerUse
	if xpPerUse <= 0 {
		xpPerUse = 10
	}
	progression := &db.SkillProgressionConfig{
		MaxLevel:    ability.MasteryMaxLevel,
		XPCurveType: ability.MasteryXPCurveType,
		XPCurveBase: ability.MasteryXPCurveBase,
	}
	if progression.MaxLevel <= 0 {
		progression.MaxLevel = 10
	}
	if progression.XPCurveBase <= 0 {
		progression.XPCurveBase = 100
	}
	if strings.TrimSpace(progression.XPCurveType) == "" {
		progression.XPCurveType = "linear"
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

	progress.XP += xpPerUse
	if progress.XP < 0 {
		progress.XP = 0
	}
	progress.Level = db.CalculateLevelFromXP(progress.XP, progression)
	if progress.Level < 1 {
		progress.Level = 1
	}

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
		log.Printf("mastery: char=%s ability=%d LEVEL UP %d->%d xp=%d",
			charID, abilityID, oldLevel, progress.Level, progress.XP)
	} else {
		log.Printf("mastery: char=%s ability=%d xp=%d level=%d",
			charID, abilityID, progress.XP, progress.Level)
	}

	if client == nil {
		client = s.findClientByCharacterID(charID)
	}
	if client != nil {
		client.sendSkillState(ctx)
	}
}
