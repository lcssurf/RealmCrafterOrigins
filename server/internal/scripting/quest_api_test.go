package scripting

import (
	"testing"

	"realm-crafter/server/internal/world"
)

type mockQuestBridge struct {
	acceptChanged  bool
	abandonChanged bool
	turnInChanged  bool
	progressChange bool
	syncErr        error

	acceptPlayerRID  uint32
	acceptQuestID    int
	abandonPlayerRID uint32
	abandonQuestID   int
	turnInPlayerRID  uint32
	turnInQuestID    int
	progressPlayerID uint32
	progressEvent    QuestProgressEvent
	syncPlayerRID    uint32
}

func (m *mockQuestBridge) Accept(playerRID uint32, questID int) (bool, error) {
	m.acceptPlayerRID = playerRID
	m.acceptQuestID = questID
	return m.acceptChanged, nil
}

func (m *mockQuestBridge) Abandon(playerRID uint32, questID int) (bool, error) {
	m.abandonPlayerRID = playerRID
	m.abandonQuestID = questID
	return m.abandonChanged, nil
}

func (m *mockQuestBridge) TurnIn(playerRID uint32, questID int) (bool, error) {
	m.turnInPlayerRID = playerRID
	m.turnInQuestID = questID
	return m.turnInChanged, nil
}

func (m *mockQuestBridge) Progress(playerRID uint32, event QuestProgressEvent) (bool, error) {
	m.progressPlayerID = playerRID
	m.progressEvent = event
	return m.progressChange, nil
}

func (m *mockQuestBridge) Sync(playerRID uint32) error {
	m.syncPlayerRID = playerRID
	return m.syncErr
}

func TestQuestAPIRoutesQuestActionsToBridge(t *testing.T) {
	reg := New(world.New())
	defer reg.Close()

	mock := &mockQuestBridge{
		acceptChanged:  true,
		abandonChanged: false,
		turnInChanged:  true,
	}
	reg.SetQuestBridge(mock)

	if err := reg.L.DoString(`a = Quest.accept(1001, 11)`); err != nil {
		t.Fatalf("Quest.accept failed: %v", err)
	}
	if got := reg.L.GetGlobal("a"); got.String() != "true" {
		t.Fatalf("Quest.accept return mismatch: got=%s want=true", got.String())
	}
	if mock.acceptPlayerRID != 1001 || mock.acceptQuestID != 11 {
		t.Fatalf("Quest.accept bridge args mismatch: rid=%d quest=%d", mock.acceptPlayerRID, mock.acceptQuestID)
	}

	if err := reg.L.DoString(`b = Quest.abandon(1002, 12)`); err != nil {
		t.Fatalf("Quest.abandon failed: %v", err)
	}
	if got := reg.L.GetGlobal("b"); got.String() != "false" {
		t.Fatalf("Quest.abandon return mismatch: got=%s want=false", got.String())
	}
	if mock.abandonPlayerRID != 1002 || mock.abandonQuestID != 12 {
		t.Fatalf("Quest.abandon bridge args mismatch: rid=%d quest=%d", mock.abandonPlayerRID, mock.abandonQuestID)
	}

	if err := reg.L.DoString(`c = Quest.turn_in(1003, 13)`); err != nil {
		t.Fatalf("Quest.turn_in failed: %v", err)
	}
	if got := reg.L.GetGlobal("c"); got.String() != "true" {
		t.Fatalf("Quest.turn_in return mismatch: got=%s want=true", got.String())
	}
	if mock.turnInPlayerRID != 1003 || mock.turnInQuestID != 13 {
		t.Fatalf("Quest.turn_in bridge args mismatch: rid=%d quest=%d", mock.turnInPlayerRID, mock.turnInQuestID)
	}
}

func TestQuestAPIProgressHelpersMapEventPayload(t *testing.T) {
	reg := New(world.New())
	defer reg.Close()

	mock := &mockQuestBridge{progressChange: true}
	reg.SetQuestBridge(mock)

	if err := reg.L.DoString(`
		ok = Quest.progress_collect(2001, 45, 3)
	`); err != nil {
		t.Fatalf("Quest.progress_collect failed: %v", err)
	}

	if got := reg.L.GetGlobal("ok"); got.String() != "true" {
		t.Fatalf("Quest.progress_collect return mismatch: got=%s want=true", got.String())
	}
	if mock.progressPlayerID != 2001 {
		t.Fatalf("progress player mismatch: got=%d want=2001", mock.progressPlayerID)
	}
	if mock.progressEvent.ObjectiveType != QuestObjectiveCollect {
		t.Fatalf("progress type mismatch: got=%d want=%d", mock.progressEvent.ObjectiveType, QuestObjectiveCollect)
	}
	if mock.progressEvent.TargetItemID != 45 {
		t.Fatalf("progress target item mismatch: got=%d want=45", mock.progressEvent.TargetItemID)
	}
	if mock.progressEvent.Delta != 3 {
		t.Fatalf("progress delta mismatch: got=%d want=3", mock.progressEvent.Delta)
	}
}

func TestQuestAPISyncAndNoBridgeFallback(t *testing.T) {
	reg := New(world.New())
	defer reg.Close()

	// No bridge configured: should fail safely and return false.
	if err := reg.L.DoString(`no_bridge_ok = Quest.sync(1)`); err != nil {
		t.Fatalf("Quest.sync without bridge should not error: %v", err)
	}
	if got := reg.L.GetGlobal("no_bridge_ok"); got.String() != "false" {
		t.Fatalf("Quest.sync without bridge mismatch: got=%s want=false", got.String())
	}

	mock := &mockQuestBridge{}
	reg.SetQuestBridge(mock)
	if err := reg.L.DoString(`sync_ok = Quest.sync(3001)`); err != nil {
		t.Fatalf("Quest.sync with bridge failed: %v", err)
	}
	if got := reg.L.GetGlobal("sync_ok"); got.String() != "true" {
		t.Fatalf("Quest.sync with bridge mismatch: got=%s want=true", got.String())
	}
	if mock.syncPlayerRID != 3001 {
		t.Fatalf("Quest.sync bridge args mismatch: rid=%d want=3001", mock.syncPlayerRID)
	}
}
