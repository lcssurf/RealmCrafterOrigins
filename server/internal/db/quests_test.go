package db

import (
	"context"
	"path/filepath"
	"testing"
)

func openTestDB(t *testing.T) *DB {
	t.Helper()
	ctx := context.Background()
	dsn := filepath.Join(t.TempDir(), "test.db")
	database, err := Open(ctx, "sqlite", dsn)
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	t.Cleanup(database.Close)
	return database
}

func createTestCharacter(t *testing.T, database *DB) string {
	t.Helper()
	ctx := context.Background()
	accountID, err := database.CreateAccount(ctx, "quest_tester", "hash", "quest@example.com")
	if err != nil {
		t.Fatalf("create account: %v", err)
	}
	if _, err := database.CreateCharacter(ctx, accountID, 0, "QuestTester", "Human", "Warrior", 0, 0, 0, 0, 0, 0); err != nil {
		t.Fatalf("create character: %v", err)
	}
	ch, err := database.GetCharacterBySlot(ctx, accountID, 0)
	if err != nil {
		t.Fatalf("get character: %v", err)
	}
	return ch.ID
}

func getSeedQuestID(t *testing.T, database *DB) int {
	t.Helper()
	ctx := context.Background()
	var questID int
	if err := database.db.QueryRowContext(ctx,
		database.q(`SELECT id FROM quest_defs WHERE code = ?`),
		"training_camp_cleanup",
	).Scan(&questID); err != nil {
		t.Fatalf("seed quest id: %v", err)
	}
	return questID
}

func completeSeedQuest(t *testing.T, database *DB, charID string) {
	t.Helper()
	ctx := context.Background()
	changedIDs, err := database.ApplyQuestProgressEvent(ctx, charID, QuestProgressEvent{
		ObjectiveType: QuestObjectiveKill,
		TargetNPCName: "Goblin",
		Delta:         999,
	})
	if err != nil {
		t.Fatalf("complete seed quest progress: %v", err)
	}
	if len(changedIDs) == 0 {
		t.Fatalf("expected quest progress changes when completing seed quest")
	}
}

func TestQuestFlowAcceptProgressTurnIn(t *testing.T) {
	ctx := context.Background()
	database := openTestDB(t)
	charID := createTestCharacter(t, database)
	questID := getSeedQuestID(t, database)

	changed, err := database.AcceptQuest(ctx, charID, questID)
	if err != nil {
		t.Fatalf("accept quest: %v", err)
	}
	if !changed {
		t.Fatalf("accept quest should change state")
	}

	logEntries, err := database.ListQuestLog(ctx, charID)
	if err != nil {
		t.Fatalf("list quest log: %v", err)
	}
	if len(logEntries) != 1 {
		t.Fatalf("unexpected quest log count: got=%d want=1", len(logEntries))
	}
	if logEntries[0].State != QuestStateActive {
		t.Fatalf("unexpected quest state after accept: got=%d want=%d", logEntries[0].State, QuestStateActive)
	}
	if len(logEntries[0].Objectives) != 1 {
		t.Fatalf("unexpected objective count: got=%d want=1", len(logEntries[0].Objectives))
	}
	if logEntries[0].Objectives[0].CurrentCount != 0 || logEntries[0].Objectives[0].TargetCount != 5 {
		t.Fatalf("unexpected objective progress after accept: got=%d/%d want=0/5",
			logEntries[0].Objectives[0].CurrentCount, logEntries[0].Objectives[0].TargetCount)
	}

	changedIDs, err := database.ApplyQuestProgressEvent(ctx, charID, QuestProgressEvent{
		ObjectiveType: QuestObjectiveKill,
		TargetNPCName: "Goblin",
		Delta:         3,
	})
	if err != nil {
		t.Fatalf("apply progress 3: %v", err)
	}
	if len(changedIDs) != 1 || changedIDs[0] != questID {
		t.Fatalf("unexpected changed quest ids after first progress: %+v", changedIDs)
	}

	changedIDs, err = database.ApplyQuestProgressEvent(ctx, charID, QuestProgressEvent{
		ObjectiveType: QuestObjectiveKill,
		TargetNPCName: "Goblin",
		Delta:         2,
	})
	if err != nil {
		t.Fatalf("apply progress 2: %v", err)
	}
	if len(changedIDs) != 1 || changedIDs[0] != questID {
		t.Fatalf("unexpected changed quest ids after second progress: %+v", changedIDs)
	}

	logEntries, err = database.ListQuestLog(ctx, charID)
	if err != nil {
		t.Fatalf("list quest log after progress: %v", err)
	}
	if logEntries[0].State != QuestStateCompleted {
		t.Fatalf("unexpected quest state after completion: got=%d want=%d", logEntries[0].State, QuestStateCompleted)
	}
	if logEntries[0].Objectives[0].CurrentCount != 5 {
		t.Fatalf("unexpected objective progress after completion: got=%d want=5", logEntries[0].Objectives[0].CurrentCount)
	}

	turnIn, changed, err := database.TurnInQuest(ctx, charID, questID)
	if err != nil {
		t.Fatalf("turn in quest: %v", err)
	}
	if !changed {
		t.Fatalf("turn in should change state")
	}
	if turnIn == nil || len(turnIn.Rewards) == 0 {
		t.Fatalf("expected seeded rewards for quest")
	}

	_, changed, err = database.TurnInQuest(ctx, charID, questID)
	if err != nil {
		t.Fatalf("idempotent turn in should not fail: %v", err)
	}
	if changed {
		t.Fatalf("second turn in should be idempotent and not change state")
	}

	logEntries, err = database.ListQuestLog(ctx, charID)
	if err != nil {
		t.Fatalf("list quest log after turn in: %v", err)
	}
	if logEntries[0].State != QuestStateTurnedIn {
		t.Fatalf("unexpected quest state after turn in: got=%d want=%d", logEntries[0].State, QuestStateTurnedIn)
	}
}

func TestQuestAcceptIdempotent(t *testing.T) {
	ctx := context.Background()
	database := openTestDB(t)
	charID := createTestCharacter(t, database)
	questID := getSeedQuestID(t, database)

	changed, err := database.AcceptQuest(ctx, charID, questID)
	if err != nil {
		t.Fatalf("accept quest first: %v", err)
	}
	if !changed {
		t.Fatalf("first accept should change state")
	}

	changed, err = database.AcceptQuest(ctx, charID, questID)
	if err != nil {
		t.Fatalf("accept quest second (idempotent): %v", err)
	}
	if changed {
		t.Fatalf("second accept should be idempotent")
	}
}

func TestQuestAvailableListLifecycle(t *testing.T) {
	ctx := context.Background()
	database := openTestDB(t)
	charID := createTestCharacter(t, database)
	questID := getSeedQuestID(t, database)

	available, err := database.ListAvailableQuests(ctx, charID)
	if err != nil {
		t.Fatalf("list available (initial): %v", err)
	}
	if len(available) == 0 {
		t.Fatalf("expected at least one available quest")
	}
	found := false
	for _, q := range available {
		if q.QuestID == questID {
			found = true
			break
		}
	}
	if !found {
		t.Fatalf("seed quest should be available before accept")
	}

	changed, err := database.AcceptQuest(ctx, charID, questID)
	if err != nil || !changed {
		t.Fatalf("accept quest: changed=%v err=%v", changed, err)
	}

	available, err = database.ListAvailableQuests(ctx, charID)
	if err != nil {
		t.Fatalf("list available (after accept): %v", err)
	}
	for _, q := range available {
		if q.QuestID == questID {
			t.Fatalf("accepted quest should not stay in available list")
		}
	}
}

func TestTurnInQuestRejectsOversizedRewardQtyAndKeepsQuestCompleted(t *testing.T) {
	ctx := context.Background()
	database := openTestDB(t)
	charID := createTestCharacter(t, database)
	questID := getSeedQuestID(t, database)

	filler, err := database.CreateItemTemplate(ctx, &ItemTemplate{
		Name:      "Overflow Reward Item",
		ItemType:  3,
		SlotType:  255,
		MaxStack:  1,
		ItemValue: 1,
		Stackable: true,
	})
	if err != nil {
		t.Fatalf("create item template: %v", err)
	}

	if _, err := database.db.ExecContext(ctx, database.q(`
		UPDATE quest_reward_defs
		   SET xp_reward = 0, gold_reward = 0, item_id = ?, item_qty = 300
		 WHERE quest_id = ?`), filler, questID); err != nil {
		t.Fatalf("update quest reward to oversized qty: %v", err)
	}

	changed, err := database.AcceptQuest(ctx, charID, questID)
	if err != nil || !changed {
		t.Fatalf("accept quest: changed=%v err=%v", changed, err)
	}
	completeSeedQuest(t, database, charID)

	if _, changed, err := database.TurnInQuest(ctx, charID, questID); err == nil {
		t.Fatalf("expected oversized reward qty turn-in to fail")
	} else if changed {
		t.Fatalf("failed turn-in must not report changed=true")
	}

	logEntries, err := database.ListQuestLog(ctx, charID)
	if err != nil {
		t.Fatalf("list quest log after failed turn-in: %v", err)
	}
	state := uint8(0)
	if len(logEntries) > 0 {
		state = logEntries[0].State
	}
	if len(logEntries) != 1 || state != QuestStateCompleted {
		t.Fatalf("quest should remain completed after failed turn-in, entries=%d state=%d",
			len(logEntries), state)
	}
}

func TestTurnInQuestFailsWhenInventoryFullAndKeepsQuestCompleted(t *testing.T) {
	ctx := context.Background()
	database := openTestDB(t)
	charID := createTestCharacter(t, database)
	questID := getSeedQuestID(t, database)

	rewardItemID, err := database.CreateItemTemplate(ctx, &ItemTemplate{
		Name:      "TurnIn Reward Item",
		ItemType:  3,
		SlotType:  255,
		MaxStack:  1,
		ItemValue: 1,
		Stackable: true,
	})
	if err != nil {
		t.Fatalf("create reward item template: %v", err)
	}
	fillerItemID, err := database.CreateItemTemplate(ctx, &ItemTemplate{
		Name:      "Inventory Filler Item",
		ItemType:  3,
		SlotType:  255,
		MaxStack:  1,
		ItemValue: 1,
		Stackable: true,
	})
	if err != nil {
		t.Fatalf("create filler item template: %v", err)
	}

	if _, err := database.db.ExecContext(ctx, database.q(`
		UPDATE quest_reward_defs
		   SET xp_reward = 0, gold_reward = 0, item_id = ?, item_qty = 1
		 WHERE quest_id = ?`), rewardItemID, questID); err != nil {
		t.Fatalf("update quest reward to item reward: %v", err)
	}

	for slot := uint8(14); slot <= 44; slot++ {
		if err := database.AddItemToSlot(ctx, charID, slot, uint16(fillerItemID), 1, 100); err != nil {
			t.Fatalf("fill inventory slot %d: %v", slot, err)
		}
	}

	changed, err := database.AcceptQuest(ctx, charID, questID)
	if err != nil || !changed {
		t.Fatalf("accept quest: changed=%v err=%v", changed, err)
	}
	completeSeedQuest(t, database, charID)

	if _, changed, err := database.TurnInQuest(ctx, charID, questID); err == nil {
		t.Fatalf("expected turn-in to fail when inventory is full")
	} else if changed {
		t.Fatalf("failed turn-in must not report changed=true")
	}

	logEntries, err := database.ListQuestLog(ctx, charID)
	if err != nil {
		t.Fatalf("list quest log after failed full-inventory turn-in: %v", err)
	}
	state := uint8(0)
	if len(logEntries) > 0 {
		state = logEntries[0].State
	}
	if len(logEntries) != 1 || state != QuestStateCompleted {
		t.Fatalf("quest should remain completed after failed full-inventory turn-in, entries=%d state=%d",
			len(logEntries), state)
	}
}
