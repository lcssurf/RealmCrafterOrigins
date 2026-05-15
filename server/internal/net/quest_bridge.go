package net

import (
	"context"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/scripting"
)

var _ scripting.QuestBridge = (*Server)(nil)

func (s *Server) withQuestClient(
	playerRID uint32,
	fn func(*ClientConn) (bool, error),
) (bool, error) {
	if s == nil || playerRID == 0 {
		return false, nil
	}
	c := s.findClientByRuntimeID(playerRID)
	if c == nil || c.actor == nil || c.actor.CharacterID == "" {
		return false, nil
	}
	return fn(c)
}

func (s *Server) Accept(playerRID uint32, questID int) (bool, error) {
	return s.withQuestClient(playerRID, func(c *ClientConn) (bool, error) {
		return c.executeQuestAction(context.Background(), protocol.QuestActionAccept, questID)
	})
}

func (s *Server) Abandon(playerRID uint32, questID int) (bool, error) {
	return s.withQuestClient(playerRID, func(c *ClientConn) (bool, error) {
		return c.executeQuestAction(context.Background(), protocol.QuestActionAbandon, questID)
	})
}

func (s *Server) TurnIn(playerRID uint32, questID int) (bool, error) {
	return s.withQuestClient(playerRID, func(c *ClientConn) (bool, error) {
		return c.executeQuestAction(context.Background(), protocol.QuestActionTurnIn, questID)
	})
}

func (s *Server) Progress(playerRID uint32, event scripting.QuestProgressEvent) (bool, error) {
	return s.withQuestClient(playerRID, func(c *ClientConn) (bool, error) {
		dbEvent := db.QuestProgressEvent{
			ObjectiveType: event.ObjectiveType,
			TargetNPCName: event.TargetNPCName,
			TargetItemID:  event.TargetItemID,
			TargetArea:    event.TargetArea,
			Delta:         event.Delta,
		}
		return c.applyQuestProgressEventResult(context.Background(), dbEvent)
	})
}

func (s *Server) Sync(playerRID uint32) error {
	_, err := s.withQuestClient(playerRID, func(c *ClientConn) (bool, error) {
		return true, c.sendQuestLogSnapshot(context.Background())
	})
	return err
}
