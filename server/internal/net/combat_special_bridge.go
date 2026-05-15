package net

import (
	"context"
	"log"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

func (s *Server) handleSpecialKill(area *world.Area, attacker *world.Actor, target *world.Actor) {
	if s == nil || area == nil || attacker == nil || target == nil {
		return
	}
	if !target.IsNPC {
		return
	}

	x, y, z := target.X, target.Y, target.Z
	area.KillNPC(target)
	area.SpawnDropsForNPC(target)

	if attacker.IsNPC {
		return
	}
	c := s.findClientByRuntimeID(attacker.RuntimeID)
	if c == nil {
		return
	}

	ctx := context.Background()
	c.applyQuestProgressEvent(ctx, db.QuestProgressEvent{
		ObjectiveType: db.QuestObjectiveKill,
		TargetNPCName: target.Name,
		Delta:         1,
	})
	c.broadcastEmitter(area, protocol.EmitterBlood, x, y, z, 0)
	c.broadcastSound(area, protocol.SoundNPCDeath, 200)
	if err := c.awardXP(ctx, int(target.Level), x, z); err != nil {
		log.Printf("special-kill: award XP failed attacker=%s target=%s err=%v", attacker.Name, target.Name, err)
	}
}
