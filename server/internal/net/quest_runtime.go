package net

import (
	"context"
	"encoding/binary"
	"hash/fnv"
	"log"
	"sort"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

const (
	questLogPacketSnapshot uint8 = 0
	questLogPacketDelta    uint8 = 1
)

type questLogSyncCache struct {
	initialized     bool
	characterID     string
	questHashes     map[int]uint64
	availableHashes map[int]uint64
}

func (c *ClientConn) resetQuestLogSyncCache() {
	c.questLogSync = questLogSyncCache{}
}

func hashQuestLogEntry(entry *db.QuestLogEntry) uint64 {
	h := fnv.New64a()
	var b4 [4]byte
	var b2 [2]byte

	writeU32 := func(v uint32) {
		binary.LittleEndian.PutUint32(b4[:], v)
		_, _ = h.Write(b4[:])
	}
	writeU16 := func(v uint16) {
		binary.LittleEndian.PutUint16(b2[:], v)
		_, _ = h.Write(b2[:])
	}
	writeU8 := func(v uint8) {
		_, _ = h.Write([]byte{v})
	}
	writeString := func(v string) {
		writeU16(uint16(len(v)))
		_, _ = h.Write([]byte(v))
	}

	writeU32(uint32(entry.QuestID))
	writeU8(entry.State)
	writeString(entry.Code)
	writeString(entry.Title)
	writeString(entry.Description)
	objectiveCount := len(entry.Objectives)
	if objectiveCount > 255 {
		objectiveCount = 255
	}
	writeU8(uint8(objectiveCount))
	for i := 0; i < objectiveCount; i++ {
		objective := entry.Objectives[i]
		writeU32(uint32(objective.ObjectiveID))
		writeU8(objective.ObjectiveType)
		writeString(objective.Description)
		writeU16(uint16(objective.CurrentCount))
		writeU16(uint16(objective.TargetCount))
		writeString(objective.TargetNPCName)
		writeU32(uint32(objective.TargetItemID))
		writeString(objective.TargetArea)
	}
	return h.Sum64()
}

func hashQuestAvailableEntry(entry *db.QuestAvailableEntry) uint64 {
	h := fnv.New64a()
	var b4 [4]byte
	var b2 [2]byte

	writeU32 := func(v uint32) {
		binary.LittleEndian.PutUint32(b4[:], v)
		_, _ = h.Write(b4[:])
	}
	writeU16 := func(v uint16) {
		binary.LittleEndian.PutUint16(b2[:], v)
		_, _ = h.Write(b2[:])
	}
	writeU8 := func(v uint8) {
		_, _ = h.Write([]byte{v})
	}
	writeString := func(v string) {
		writeU16(uint16(len(v)))
		_, _ = h.Write([]byte(v))
	}

	writeU32(uint32(entry.QuestID))
	writeString(entry.Code)
	writeString(entry.Title)
	writeString(entry.Description)
	writeU16(uint16(entry.MinLevel))
	if entry.Repeatable {
		writeU8(1)
	} else {
		writeU8(0)
	}
	return h.Sum64()
}

func buildQuestHashes(entries []*db.QuestLogEntry) map[int]uint64 {
	out := make(map[int]uint64, len(entries))
	for _, entry := range entries {
		out[entry.QuestID] = hashQuestLogEntry(entry)
	}
	return out
}

func buildAvailableQuestHashes(entries []*db.QuestAvailableEntry) map[int]uint64 {
	out := make(map[int]uint64, len(entries))
	for _, entry := range entries {
		out[entry.QuestID] = hashQuestAvailableEntry(entry)
	}
	return out
}

func writeQuestLogEntry(w *Writer, entry *db.QuestLogEntry) {
	w.WriteUint32(uint32(entry.QuestID))
	w.WriteUint8(entry.State)
	w.WriteString(entry.Code)
	w.WriteString(entry.Title)
	w.WriteString(entry.Description)
	objectiveCount := len(entry.Objectives)
	if objectiveCount > 255 {
		objectiveCount = 255
	}
	w.WriteUint8(uint8(objectiveCount))
	for i := 0; i < objectiveCount; i++ {
		objective := entry.Objectives[i]
		w.WriteUint32(uint32(objective.ObjectiveID))
		w.WriteUint8(objective.ObjectiveType)
		w.WriteString(objective.Description)
		w.WriteUint16(uint16(objective.CurrentCount))
		w.WriteUint16(uint16(objective.TargetCount))
	}
}

func writeQuestAvailableEntry(w *Writer, entry *db.QuestAvailableEntry) {
	w.WriteUint32(uint32(entry.QuestID))
	w.WriteString(entry.Code)
	w.WriteString(entry.Title)
	w.WriteString(entry.Description)
	w.WriteUint16(uint16(entry.MinLevel))
	w.WriteBool(entry.Repeatable)
}

func (c *ClientConn) sendQuestLogSnapshotPacket(
	logEntries []*db.QuestLogEntry,
	availableEntries []*db.QuestAvailableEntry,
) error {
	var w Writer
	w.WriteUint8(questLogPacketSnapshot)
	w.WriteUint16(uint16(len(logEntries)))
	for _, entry := range logEntries {
		writeQuestLogEntry(&w, entry)
	}
	w.WriteUint16(uint16(len(availableEntries)))
	for _, entry := range availableEntries {
		writeQuestAvailableEntry(&w, entry)
	}
	return c.sendPacket(protocol.PQuestLog, w.Bytes())
}

func (c *ClientConn) sendQuestLogDeltaPacket(
	questUpserts []*db.QuestLogEntry,
	questRemovals []int,
	availableUpserts []*db.QuestAvailableEntry,
	availableRemovals []int,
) error {
	var w Writer
	w.WriteUint8(questLogPacketDelta)

	w.WriteUint16(uint16(len(questUpserts)))
	for _, entry := range questUpserts {
		writeQuestLogEntry(&w, entry)
	}
	w.WriteUint16(uint16(len(questRemovals)))
	for _, questID := range questRemovals {
		w.WriteUint32(uint32(questID))
	}

	w.WriteUint16(uint16(len(availableUpserts)))
	for _, entry := range availableUpserts {
		writeQuestAvailableEntry(&w, entry)
	}
	w.WriteUint16(uint16(len(availableRemovals)))
	for _, questID := range availableRemovals {
		w.WriteUint32(uint32(questID))
	}
	return c.sendPacket(protocol.PQuestLog, w.Bytes())
}

// sendQuestLogSnapshot pushes quest updates to the client.
//
// Wire format (v3):
//
// Snapshot mode:
//
//	mode(u8=0) + quest_count(u16)
//	  quest_id(u32) + state(u8) + code(str) + title(str) + description(str) + objective_count(u8)
//	    objective_id(u32) + objective_type(u8) + description(str) + current(u16) + target(u16)
//	available_count(u16)
//	  quest_id(u32) + code(str) + title(str) + description(str) + min_level(u16) + repeatable(u8)
//
// Delta mode:
//
//	mode(u8=1)
//	quest_upsert_count(u16)
//	  quest_entry (same fields as snapshot quest row)
//	quest_remove_count(u16)
//	  quest_id(u32)
//	available_upsert_count(u16)
//	  available_entry (same fields as snapshot available row)
//	available_remove_count(u16)
//	  quest_id(u32)
func (c *ClientConn) sendQuestLogSnapshot(ctx context.Context) error {
	if c.server == nil || c.server.db == nil || c.actor == nil || c.actor.CharacterID == "" {
		return nil
	}
	if c.questLogSync.characterID != c.actor.CharacterID {
		c.resetQuestLogSyncCache()
		c.questLogSync.characterID = c.actor.CharacterID
	}

	logEntries, err := c.server.db.ListQuestLog(ctx, c.actor.CharacterID)
	if err != nil {
		return err
	}
	availableEntries, err := c.server.db.ListAvailableQuests(ctx, c.actor.CharacterID)
	if err != nil {
		return err
	}

	currentQuestHashes := buildQuestHashes(logEntries)
	currentAvailableHashes := buildAvailableQuestHashes(availableEntries)

	if !c.questLogSync.initialized {
		if err := c.sendQuestLogSnapshotPacket(logEntries, availableEntries); err != nil {
			return err
		}
		c.questLogSync.initialized = true
		c.questLogSync.questHashes = currentQuestHashes
		c.questLogSync.availableHashes = currentAvailableHashes
		return nil
	}

	var questUpserts []*db.QuestLogEntry
	for _, entry := range logEntries {
		newHash := currentQuestHashes[entry.QuestID]
		oldHash, exists := c.questLogSync.questHashes[entry.QuestID]
		if !exists || oldHash != newHash {
			questUpserts = append(questUpserts, entry)
		}
	}
	var questRemovals []int
	for questID := range c.questLogSync.questHashes {
		if _, exists := currentQuestHashes[questID]; !exists {
			questRemovals = append(questRemovals, questID)
		}
	}
	sort.Ints(questRemovals)

	var availableUpserts []*db.QuestAvailableEntry
	for _, entry := range availableEntries {
		newHash := currentAvailableHashes[entry.QuestID]
		oldHash, exists := c.questLogSync.availableHashes[entry.QuestID]
		if !exists || oldHash != newHash {
			availableUpserts = append(availableUpserts, entry)
		}
	}
	var availableRemovals []int
	for questID := range c.questLogSync.availableHashes {
		if _, exists := currentAvailableHashes[questID]; !exists {
			availableRemovals = append(availableRemovals, questID)
		}
	}
	sort.Ints(availableRemovals)

	hasChanges :=
		len(questUpserts) > 0 ||
			len(questRemovals) > 0 ||
			len(availableUpserts) > 0 ||
			len(availableRemovals) > 0
	if !hasChanges {
		return nil
	}

	if err := c.sendQuestLogDeltaPacket(
		questUpserts,
		questRemovals,
		availableUpserts,
		availableRemovals,
	); err != nil {
		return err
	}

	c.questLogSync.questHashes = currentQuestHashes
	c.questLogSync.availableHashes = currentAvailableHashes
	return nil
}

// executeQuestAction applies one quest command and pushes resulting packets.
// action must be one of protocol.QuestAction* constants.
func (c *ClientConn) executeQuestAction(ctx context.Context, action uint8, questID int) (bool, error) {
	if c.server == nil || c.server.db == nil || c.actor == nil || c.actor.CharacterID == "" || questID <= 0 {
		return false, nil
	}

	questChanged := false

	switch action {
	case protocol.QuestActionAccept:
		changed, err := c.server.db.AcceptQuest(ctx, c.actor.CharacterID, questID)
		if err != nil {
			return false, err
		}
		questChanged = changed

	case protocol.QuestActionAbandon:
		changed, err := c.server.db.AbandonQuest(ctx, c.actor.CharacterID, questID)
		if err != nil {
			return false, err
		}
		questChanged = changed

	case protocol.QuestActionTurnIn:
		turnIn, changed, err := c.server.db.TurnInQuest(ctx, c.actor.CharacterID, questID)
		if err != nil {
			return false, err
		}
		questChanged = changed
		if questChanged && turnIn != nil {
			c.applyQuestTurnInResult(ctx, turnIn)
		}

	default:
		return false, nil
	}

	if questChanged {
		if err := c.sendQuestLogSnapshot(ctx); err != nil {
			return true, err
		}
	}
	return questChanged, nil
}

// applyQuestTurnInResult mirrors DB turn-in results into runtime actor state
// and sends inventory/gold/xp packets when relevant.
func (c *ClientConn) applyQuestTurnInResult(ctx context.Context, turnIn *db.QuestTurnInResult) {
	if c.actor == nil || turnIn == nil {
		return
	}

	leveled := false
	c.actor.Mu.Lock()
	if turnIn.GoldChanged {
		c.actor.Gold = turnIn.NewGold
	}
	if turnIn.XPChanged {
		c.actor.XP = turnIn.NewXP
		c.actor.Level = uint16(turnIn.NewLevel)
		if turnIn.Leveled {
			leveled = true
			c.actor.HealthMax = turnIn.NewHPMax
			c.actor.EnergyMax = turnIn.NewEPMax
			c.actor.Health = turnIn.NewHPMax
			c.actor.Energy = turnIn.NewEPMax
			c.actor.Strength = turnIn.NewStrength
		}
	}
	c.actor.Mu.Unlock()
	if leveled {
		c.actor.SetPrimaryStats(c.primaryStatsForLevel(ctx, turnIn.NewLevel))
		world.RecomputeDerivedStats(c.actor)
		c.actor.Mu.Lock()
		c.actor.Health = c.actor.HealthMax
		c.actor.Energy = c.actor.EnergyMax
		c.actor.Mu.Unlock()
	}

	if turnIn.ItemsChanged {
		if err := c.sendInventory(ctx, c.actor.CharacterID); err != nil {
			log.Printf("quest-runtime: turn-in inventory send failed char=%q err=%v", c.actor.Name, err)
		}
	}
	if turnIn.GoldChanged {
		c.sendGoldUpdate()
	}
	if turnIn.XPChanged {
		if err := c.sendXPUpdate(); err != nil {
			log.Printf("quest-runtime: turn-in xp send failed char=%q err=%v", c.actor.Name, err)
		}
	}
}

func (c *ClientConn) applyQuestProgressEventResult(
	ctx context.Context,
	event db.QuestProgressEvent,
) (bool, error) {
	if c.server == nil || c.server.db == nil || c.actor == nil || c.actor.CharacterID == "" {
		return false, nil
	}
	changedQuestIDs, err := c.server.db.ApplyQuestProgressEvent(ctx, c.actor.CharacterID, event)
	if err != nil {
		return false, err
	}
	if len(changedQuestIDs) == 0 {
		return false, nil
	}
	if err := c.sendQuestLogSnapshot(ctx); err != nil {
		return true, err
	}
	return true, nil
}

// applyQuestProgressEvent advances objective progress and pushes an updated
// quest log packet when anything changed.
func (c *ClientConn) applyQuestProgressEvent(ctx context.Context, event db.QuestProgressEvent) {
	changed, err := c.applyQuestProgressEventResult(ctx, event)
	if err != nil {
		log.Printf("quest-progress: apply failed char=%q type=%d err=%v",
			c.actor.Name, event.ObjectiveType, err)
		return
	}
	if !changed {
		return
	}
}

// applyQuestRewards grants XP, gold and item rewards after a successful turn-in.
func (c *ClientConn) applyQuestRewards(ctx context.Context, rewards []db.QuestRewardEntry) {
	if c.server == nil || c.server.db == nil || c.actor == nil || c.actor.CharacterID == "" || len(rewards) == 0 {
		return
	}

	var (
		totalXP      int64
		totalGold    int64
		changedItems bool
		changedGold  bool
	)

	for _, reward := range rewards {
		totalXP += reward.XPReward
		totalGold += reward.GoldReward
		if reward.ItemID > 0 && reward.ItemQty > 0 {
			if _, err := c.server.db.AddStackableItem(ctx, c.actor.CharacterID, reward.ItemID, reward.ItemQty, 0, 100); err != nil {
				log.Printf("quest-reward: add item failed char=%q item=%d qty=%d err=%v",
					c.actor.Name, reward.ItemID, reward.ItemQty, err)
			} else {
				changedItems = true
			}
		}
	}

	if totalGold != 0 {
		newGold, err := c.server.db.UpdateGold(ctx, c.actor.CharacterID, totalGold)
		if err != nil {
			log.Printf("quest-reward: update gold failed char=%q delta=%d err=%v",
				c.actor.Name, totalGold, err)
		} else {
			c.actor.Mu.Lock()
			c.actor.Gold = newGold
			c.actor.Mu.Unlock()
			changedGold = true
		}
	}

	if totalXP > 0 {
		if err := c.awardXPAmount(ctx, totalXP); err != nil {
			log.Printf("quest-reward: update xp failed char=%q gain=%d err=%v",
				c.actor.Name, totalXP, err)
		}
	}

	if changedGold {
		c.sendGoldUpdate()
	}
	if changedItems {
		if err := c.sendInventory(ctx, c.actor.CharacterID); err != nil {
			log.Printf("quest-reward: inventory send failed char=%q err=%v", c.actor.Name, err)
		}
	}
}

// awardXPAmount applies a raw XP gain (used by quest rewards).
func (c *ClientConn) awardXPAmount(ctx context.Context, gain int64) error {
	if gain <= 0 {
		return nil
	}

	c.actor.Mu.Lock()
	curXP := c.actor.XP
	curLevel := int(c.actor.Level)
	c.actor.Mu.Unlock()

	newXP, newLevel, leveled := world.ProcessXPCumulative(curXP, curLevel, gain)
	hpMax, epMax, strength := world.StatsByLevel(newLevel)

	if err := c.server.db.SaveXP(ctx, c.actor.CharacterID, newXP, newLevel, hpMax, epMax); err != nil {
		return err
	}

	c.actor.Mu.Lock()
	c.actor.XP = newXP
	c.actor.Level = uint16(newLevel)
	if leveled {
		c.actor.HealthMax = hpMax
		c.actor.EnergyMax = epMax
		c.actor.Health = hpMax
		c.actor.Energy = epMax
		c.actor.Strength = strength
	}
	c.actor.Mu.Unlock()
	if leveled {
		c.actor.SetPrimaryStats(c.primaryStatsForLevel(ctx, newLevel))
		world.RecomputeDerivedStats(c.actor)
		c.actor.Mu.Lock()
		c.actor.Health = c.actor.HealthMax
		c.actor.Energy = c.actor.EnergyMax
		c.actor.Mu.Unlock()
	}
	return c.sendXPUpdate()
}
