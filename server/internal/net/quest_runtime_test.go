package net

import (
	"testing"

	"realm-crafter/server/internal/db"
)

func TestWriteQuestLogEntryClampsObjectiveCountToU8(t *testing.T) {
	entry := &db.QuestLogEntry{
		QuestID:     7,
		State:       db.QuestStateActive,
		Code:        "stress_objective_count",
		Title:       "Stress Objective Count",
		Description: "Quest with many objectives.",
	}
	for i := 0; i < 300; i++ {
		entry.Objectives = append(entry.Objectives, db.QuestLogObjective{
			ObjectiveID:   i + 1,
			ObjectiveType: db.QuestObjectiveKill,
			Description:   "Objective",
			CurrentCount:  i,
			TargetCount:   i + 1,
		})
	}

	var w Writer
	writeQuestLogEntry(&w, entry)
	payload := w.Bytes()

	r := NewReader(payload)
	if _, err := r.ReadUint32(); err != nil {
		t.Fatalf("read quest id: %v", err)
	}
	if _, err := r.ReadUint8(); err != nil {
		t.Fatalf("read state: %v", err)
	}
	if _, err := r.ReadString(); err != nil {
		t.Fatalf("read code: %v", err)
	}
	if _, err := r.ReadString(); err != nil {
		t.Fatalf("read title: %v", err)
	}
	if _, err := r.ReadString(); err != nil {
		t.Fatalf("read description: %v", err)
	}

	objectiveCount, err := r.ReadUint8()
	if err != nil {
		t.Fatalf("read objective count: %v", err)
	}
	if objectiveCount != 255 {
		t.Fatalf("expected objective count clamp to 255, got %d", objectiveCount)
	}

	for i := 0; i < int(objectiveCount); i++ {
		if _, err := r.ReadUint32(); err != nil {
			t.Fatalf("read objective[%d] id: %v", i, err)
		}
		if _, err := r.ReadUint8(); err != nil {
			t.Fatalf("read objective[%d] type: %v", i, err)
		}
		if _, err := r.ReadString(); err != nil {
			t.Fatalf("read objective[%d] description: %v", i, err)
		}
		if _, err := r.ReadUint16(); err != nil {
			t.Fatalf("read objective[%d] current: %v", i, err)
		}
		if _, err := r.ReadUint16(); err != nil {
			t.Fatalf("read objective[%d] target: %v", i, err)
		}
	}

	if _, err := r.ReadUint8(); err == nil {
		t.Fatalf("expected EOF after clamped objective payload")
	}
}
