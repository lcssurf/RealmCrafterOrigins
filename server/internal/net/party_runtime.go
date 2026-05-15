package net

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/fnv"
	"sort"
	"strings"
	"sync"

	"realm-crafter/server/internal/protocol"
)

const (
	partyUpdateModeSnapshot = 0
	partyUpdateModeDelta    = 1
)

type partyState struct {
	ID                uint32
	LeaderCharacterID string
	Members           map[string]struct{} // key: character_id
}

type partyInvite struct {
	FromCharacterID string
	FromName        string
	PartyID         uint32
}

type partySnapshot struct {
	PartyID            uint32
	LeaderCharacterID  string
	MemberCharacterIDs []string
	PendingInviteFrom  string
}

type partyMemberWire struct {
	RuntimeID uint32
	Name      string
	Level     uint16
	HP        uint16
	HPMax     uint16
	Online    bool
}

type partySyncCache struct {
	initialized       bool
	partyID           uint32
	leaderRID         uint32
	pendingInviteFrom string
	memberHashes      map[uint32]uint64
}

var (
	errPartyInvalidPayload   = errors.New("party: invalid payload")
	errPartyNoPendingInvite  = errors.New("party: no pending invite")
	errPartyNotInParty       = errors.New("party: not in party")
	errPartyPartyGone        = errors.New("party: party no longer exists")
	errPartyNotLeader        = errors.New("party: only leader can perform this action")
	errPartyTargetInParty    = errors.New("party: target is already in a party")
	errPartyPartyFull        = errors.New("party: party is full")
	errPartyInvitePending    = errors.New("party: target already has a pending invite")
	errPartyTargetNotInParty = errors.New("party: target is not in your party")
	errPartySelfInvite       = errors.New("party: cannot invite yourself")
	errPartyKickSelf         = errors.New("party: cannot kick yourself")
)

func partyErrorCode(err error) uint8 {
	switch {
	case errors.Is(err, errPartyNoPendingInvite):
		return protocol.PartyNoticeErrorNoPendingInvite
	case errors.Is(err, errPartyNotInParty):
		return protocol.PartyNoticeErrorNotInParty
	case errors.Is(err, errPartyNotLeader):
		return protocol.PartyNoticeErrorNotLeader
	case errors.Is(err, errPartyPartyFull):
		return protocol.PartyNoticeErrorPartyFull
	case errors.Is(err, errPartyTargetInParty):
		return protocol.PartyNoticeErrorAlreadyInParty
	case errors.Is(err, errPartyInvitePending):
		return protocol.PartyNoticeErrorInvitePending
	case errors.Is(err, errPartyTargetNotInParty):
		return protocol.PartyNoticeErrorTargetNotInParty
	case errors.Is(err, errPartySelfInvite):
		return protocol.PartyNoticeErrorCannotInviteSelf
	case errors.Is(err, errPartyKickSelf):
		return protocol.PartyNoticeErrorInvalidPayload
	case errors.Is(err, errPartyPartyGone):
		return protocol.PartyNoticeErrorPartyGone
	case errors.Is(err, errPartyInvalidPayload):
		return protocol.PartyNoticeErrorInvalidPayload
	default:
		return protocol.PartyNoticeErrorInvalidPayload
	}
}

func partyErrorText(err error) string {
	switch {
	case errors.Is(err, errPartyNoPendingInvite):
		return "No pending party invite."
	case errors.Is(err, errPartyNotInParty):
		return "You are not in a party."
	case errors.Is(err, errPartyNotLeader):
		return "Only the party leader can do this."
	case errors.Is(err, errPartyPartyFull):
		return "Party is full."
	case errors.Is(err, errPartyTargetInParty):
		return "Target player is already in a party."
	case errors.Is(err, errPartyInvitePending):
		return "Target already has a pending party invite."
	case errors.Is(err, errPartyTargetNotInParty):
		return "Target is not in your party."
	case errors.Is(err, errPartySelfInvite):
		return "You cannot invite yourself."
	case errors.Is(err, errPartyKickSelf):
		return "You cannot kick yourself."
	case errors.Is(err, errPartyPartyGone):
		return "Party no longer exists."
	default:
		return "Invalid party action."
	}
}

type partyManager struct {
	mu sync.Mutex

	nextPartyID uint32
	maxMembers  int

	parties         map[uint32]*partyState
	memberToParty   map[string]uint32
	invitesByTarget map[string]partyInvite // key: target character_id
}

func newPartyManager(maxMembers int) *partyManager {
	if maxMembers < 2 {
		maxMembers = 2
	}
	return &partyManager{
		nextPartyID:     0,
		maxMembers:      maxMembers,
		parties:         make(map[uint32]*partyState),
		memberToParty:   make(map[string]uint32),
		invitesByTarget: make(map[string]partyInvite),
	}
}

func (pm *partyManager) ensurePartyFor(characterID string) *partyState {
	if partyID, ok := pm.memberToParty[characterID]; ok && partyID != 0 {
		if party, ok := pm.parties[partyID]; ok {
			return party
		}
		delete(pm.memberToParty, characterID)
	}

	pm.nextPartyID++
	party := &partyState{
		ID:                pm.nextPartyID,
		LeaderCharacterID: characterID,
		Members: map[string]struct{}{
			characterID: {},
		},
	}
	pm.parties[party.ID] = party
	pm.memberToParty[characterID] = party.ID
	return party
}

func (pm *partyManager) removeMemberFromPartyLocked(party *partyState, characterID string) {
	if party == nil || characterID == "" {
		return
	}
	delete(party.Members, characterID)
	delete(pm.memberToParty, characterID)
	if party.LeaderCharacterID == characterID {
		party.LeaderCharacterID = ""
		for memberID := range party.Members {
			party.LeaderCharacterID = memberID
			break
		}
	}
	if len(party.Members) == 0 {
		delete(pm.parties, party.ID)
	}
}

func (pm *partyManager) partyMemberIDsLocked(partyID uint32) []string {
	party := pm.parties[partyID]
	if party == nil {
		return nil
	}
	out := make([]string, 0, len(party.Members))
	for characterID := range party.Members {
		out = append(out, characterID)
	}
	sort.Strings(out)
	return out
}

func (pm *partyManager) invite(inviterCharacterID, inviterName, targetCharacterID string) (uint32, error) {
	if inviterCharacterID == "" || targetCharacterID == "" {
		return 0, errPartyInvalidPayload
	}
	if inviterCharacterID == targetCharacterID {
		return 0, errPartySelfInvite
	}

	pm.mu.Lock()
	defer pm.mu.Unlock()

	if _, ok := pm.memberToParty[targetCharacterID]; ok {
		return 0, errPartyTargetInParty
	}

	if existing, ok := pm.invitesByTarget[targetCharacterID]; ok {
		if existing.FromCharacterID == inviterCharacterID {
			return existing.PartyID, nil
		}
		return 0, errPartyInvitePending
	}

	party := pm.ensurePartyFor(inviterCharacterID)
	if party.LeaderCharacterID != inviterCharacterID {
		return 0, errPartyNotLeader
	}
	if len(party.Members) >= pm.maxMembers {
		return 0, errPartyPartyFull
	}

	pm.invitesByTarget[targetCharacterID] = partyInvite{
		FromCharacterID: inviterCharacterID,
		FromName:        inviterName,
		PartyID:         party.ID,
	}
	return party.ID, nil
}

func (pm *partyManager) accept(targetCharacterID string) (uint32, error) {
	if targetCharacterID == "" {
		return 0, errPartyInvalidPayload
	}

	pm.mu.Lock()
	defer pm.mu.Unlock()

	invite, ok := pm.invitesByTarget[targetCharacterID]
	if !ok {
		return 0, errPartyNoPendingInvite
	}

	if _, inParty := pm.memberToParty[targetCharacterID]; inParty {
		delete(pm.invitesByTarget, targetCharacterID)
		return 0, errPartyTargetInParty
	}

	party := pm.parties[invite.PartyID]
	if party == nil {
		delete(pm.invitesByTarget, targetCharacterID)
		return 0, errPartyPartyGone
	}
	if _, inviterInParty := party.Members[invite.FromCharacterID]; !inviterInParty {
		delete(pm.invitesByTarget, targetCharacterID)
		return 0, errPartyPartyGone
	}
	if len(party.Members) >= pm.maxMembers {
		return 0, errPartyPartyFull
	}

	party.Members[targetCharacterID] = struct{}{}
	pm.memberToParty[targetCharacterID] = party.ID
	delete(pm.invitesByTarget, targetCharacterID)
	return party.ID, nil
}

func (pm *partyManager) decline(targetCharacterID string) (string, error) {
	if targetCharacterID == "" {
		return "", errPartyInvalidPayload
	}

	pm.mu.Lock()
	defer pm.mu.Unlock()

	invite, ok := pm.invitesByTarget[targetCharacterID]
	if !ok {
		return "", errPartyNoPendingInvite
	}
	delete(pm.invitesByTarget, targetCharacterID)
	return invite.FromCharacterID, nil
}

func (pm *partyManager) leave(characterID string) (uint32, []string, []string, error) {
	if characterID == "" {
		return 0, nil, nil, errPartyInvalidPayload
	}

	pm.mu.Lock()
	defer pm.mu.Unlock()

	partyID, ok := pm.memberToParty[characterID]
	if !ok || partyID == 0 {
		return 0, nil, nil, errPartyNotInParty
	}
	party := pm.parties[partyID]
	if party == nil {
		delete(pm.memberToParty, characterID)
		return 0, nil, nil, errPartyPartyGone
	}

	pm.removeMemberFromPartyLocked(party, characterID)
	delete(pm.invitesByTarget, characterID)
	clearedInviteTargets := make([]string, 0)
	for targetID, invite := range pm.invitesByTarget {
		if invite.FromCharacterID == characterID {
			delete(pm.invitesByTarget, targetID)
			clearedInviteTargets = append(clearedInviteTargets, targetID)
		}
	}
	sort.Strings(clearedInviteTargets)
	return partyID, pm.partyMemberIDsLocked(partyID), clearedInviteTargets, nil
}

func (pm *partyManager) kick(requesterCharacterID, targetCharacterID string) (uint32, []string, error) {
	if requesterCharacterID == "" || targetCharacterID == "" {
		return 0, nil, errPartyInvalidPayload
	}
	if requesterCharacterID == targetCharacterID {
		return 0, nil, errPartyKickSelf
	}

	pm.mu.Lock()
	defer pm.mu.Unlock()

	partyID, ok := pm.memberToParty[requesterCharacterID]
	if !ok || partyID == 0 {
		return 0, nil, errPartyNotInParty
	}
	party := pm.parties[partyID]
	if party == nil {
		return 0, nil, errPartyPartyGone
	}
	if party.LeaderCharacterID != requesterCharacterID {
		return 0, nil, errPartyNotLeader
	}
	targetPartyID := pm.memberToParty[targetCharacterID]
	if targetPartyID != partyID {
		return 0, nil, errPartyTargetNotInParty
	}

	pm.removeMemberFromPartyLocked(party, targetCharacterID)
	delete(pm.invitesByTarget, targetCharacterID)
	for inviteTargetID, invite := range pm.invitesByTarget {
		if invite.FromCharacterID == targetCharacterID {
			delete(pm.invitesByTarget, inviteTargetID)
		}
	}
	return partyID, pm.partyMemberIDsLocked(partyID), nil
}

func (pm *partyManager) transferLeader(requesterCharacterID, targetCharacterID string) (uint32, []string, error) {
	if requesterCharacterID == "" || targetCharacterID == "" {
		return 0, nil, errPartyInvalidPayload
	}

	pm.mu.Lock()
	defer pm.mu.Unlock()

	partyID, ok := pm.memberToParty[requesterCharacterID]
	if !ok || partyID == 0 {
		return 0, nil, errPartyNotInParty
	}
	party := pm.parties[partyID]
	if party == nil {
		return 0, nil, errPartyPartyGone
	}
	if party.LeaderCharacterID != requesterCharacterID {
		return 0, nil, errPartyNotLeader
	}
	if _, exists := party.Members[targetCharacterID]; !exists {
		return 0, nil, errPartyTargetNotInParty
	}

	party.LeaderCharacterID = targetCharacterID
	return partyID, pm.partyMemberIDsLocked(partyID), nil
}

func (pm *partyManager) snapshotFor(characterID string) partySnapshot {
	pm.mu.Lock()
	defer pm.mu.Unlock()

	snapshot := partySnapshot{}
	if characterID == "" {
		return snapshot
	}

	if invite, ok := pm.invitesByTarget[characterID]; ok {
		snapshot.PendingInviteFrom = invite.FromName
	}

	partyID := pm.memberToParty[characterID]
	if partyID == 0 {
		return snapshot
	}
	party := pm.parties[partyID]
	if party == nil {
		delete(pm.memberToParty, characterID)
		return snapshot
	}

	snapshot.PartyID = party.ID
	snapshot.LeaderCharacterID = party.LeaderCharacterID
	snapshot.MemberCharacterIDs = make([]string, 0, len(party.Members))
	for memberID := range party.Members {
		snapshot.MemberCharacterIDs = append(snapshot.MemberCharacterIDs, memberID)
	}
	sort.Strings(snapshot.MemberCharacterIDs)
	return snapshot
}

func (pm *partyManager) partyMemberIDsLockedForRead(partyID uint32) []string {
	pm.mu.Lock()
	defer pm.mu.Unlock()
	return pm.partyMemberIDsLocked(partyID)
}

func clampInt32ToU16(v int32) uint16 {
	if v <= 0 {
		return 0
	}
	if v >= 65535 {
		return 65535
	}
	return uint16(v)
}

func (c *ClientConn) resetPartySyncCache() {
	c.partySync = partySyncCache{}
}

func writePartyMemberRow(w *Writer, member partyMemberWire) {
	w.WriteUint32(member.RuntimeID)
	w.WriteString(member.Name)
	w.WriteUint16(member.Level)
	w.WriteUint16(member.HP)
	w.WriteUint16(member.HPMax)
	w.WriteBool(member.Online)
}

func hashPartyMember(member partyMemberWire) uint64 {
	h := fnv.New64a()
	var b4 [4]byte
	var b2 [2]byte

	binary.LittleEndian.PutUint32(b4[:], member.RuntimeID)
	_, _ = h.Write(b4[:])
	binary.LittleEndian.PutUint16(b2[:], member.Level)
	_, _ = h.Write(b2[:])
	binary.LittleEndian.PutUint16(b2[:], member.HP)
	_, _ = h.Write(b2[:])
	binary.LittleEndian.PutUint16(b2[:], member.HPMax)
	_, _ = h.Write(b2[:])
	if member.Online {
		_, _ = h.Write([]byte{1})
	} else {
		_, _ = h.Write([]byte{0})
	}
	binary.LittleEndian.PutUint16(b2[:], uint16(len(member.Name)))
	_, _ = h.Write(b2[:])
	_, _ = h.Write([]byte(member.Name))
	return h.Sum64()
}

func (s *Server) collectPartyMembers(snapshot partySnapshot) (uint32, []partyMemberWire, map[uint32]uint64) {
	memberConns := make([]*ClientConn, 0, len(snapshot.MemberCharacterIDs))
	var leaderRID uint32
	for _, memberCharacterID := range snapshot.MemberCharacterIDs {
		memberConn := s.findClientByCharacterID(memberCharacterID)
		if memberConn == nil || memberConn.actor == nil {
			continue
		}
		if memberCharacterID == snapshot.LeaderCharacterID {
			leaderRID = memberConn.actor.RuntimeID
		}
		memberConns = append(memberConns, memberConn)
	}
	sort.Slice(memberConns, func(i, j int) bool {
		left := strings.ToLower(memberConns[i].actor.Name)
		right := strings.ToLower(memberConns[j].actor.Name)
		return left < right
	})

	members := make([]partyMemberWire, 0, len(memberConns))
	memberHashes := make(map[uint32]uint64, len(memberConns))
	for _, memberConn := range memberConns {
		actor := memberConn.actor
		actor.Mu.Lock()
		row := partyMemberWire{
			RuntimeID: actor.RuntimeID,
			Name:      actor.Name,
			Level:     actor.Level,
			HP:        clampInt32ToU16(actor.Health),
			HPMax:     clampInt32ToU16(actor.HealthMax),
			Online:    true,
		}
		actor.Mu.Unlock()
		members = append(members, row)
		memberHashes[row.RuntimeID] = hashPartyMember(row)
	}
	return leaderRID, members, memberHashes
}

func (c *ClientConn) updatePartySyncCache(
	snapshot partySnapshot,
	leaderRID uint32,
	pendingInviteFrom string,
	memberHashes map[uint32]uint64,
) {
	c.partySync.initialized = true
	c.partySync.partyID = snapshot.PartyID
	c.partySync.leaderRID = leaderRID
	c.partySync.pendingInviteFrom = pendingInviteFrom
	c.partySync.memberHashes = memberHashes
}

func (s *Server) sendPartySnapshotToClient(c *ClientConn, noticeCode uint8, notice string) error {
	if s == nil || c == nil || c.actor == nil || c.state != StateInGame {
		return nil
	}
	snapshot := s.party.snapshotFor(c.actor.CharacterID)
	leaderRID, members, memberHashes := s.collectPartyMembers(snapshot)

	shouldSendSnapshot := !c.partySync.initialized || c.partySync.partyID != snapshot.PartyID
	if shouldSendSnapshot {
		var w Writer
		w.WriteUint8(partyUpdateModeSnapshot)
		w.WriteUint32(snapshot.PartyID)
		w.WriteUint32(leaderRID)
		w.WriteUint8(uint8(len(members)))
		for _, member := range members {
			writePartyMemberRow(&w, member)
		}
		w.WriteString(snapshot.PendingInviteFrom)
		w.WriteUint8(noticeCode)
		w.WriteString(notice)
		if err := c.sendPacket(protocol.PPartyUpdate, w.Bytes()); err != nil {
			return err
		}
		c.updatePartySyncCache(snapshot, leaderRID, snapshot.PendingInviteFrom, memberHashes)
		return nil
	}

	var (
		memberUpserts []partyMemberWire
		memberRemovals []uint32
	)
	for _, member := range members {
		newHash := memberHashes[member.RuntimeID]
		oldHash, exists := c.partySync.memberHashes[member.RuntimeID]
		if !exists || oldHash != newHash {
			memberUpserts = append(memberUpserts, member)
		}
	}
	for rid := range c.partySync.memberHashes {
		if _, exists := memberHashes[rid]; !exists {
			memberRemovals = append(memberRemovals, rid)
		}
	}
	sort.Slice(memberRemovals, func(i, j int) bool { return memberRemovals[i] < memberRemovals[j] })

	hasChanges := len(memberUpserts) > 0 ||
		len(memberRemovals) > 0 ||
		c.partySync.leaderRID != leaderRID ||
		c.partySync.pendingInviteFrom != snapshot.PendingInviteFrom ||
		noticeCode != protocol.PartyNoticeNone ||
		notice != ""
	if !hasChanges {
		return nil
	}

	var w Writer
	w.WriteUint8(partyUpdateModeDelta)
	w.WriteUint32(snapshot.PartyID)
	w.WriteUint32(leaderRID)
	w.WriteUint8(uint8(len(memberUpserts)))
	for _, member := range memberUpserts {
		writePartyMemberRow(&w, member)
	}
	w.WriteUint8(uint8(len(memberRemovals)))
	for _, rid := range memberRemovals {
		w.WriteUint32(rid)
	}
	w.WriteString(snapshot.PendingInviteFrom)
	w.WriteUint8(noticeCode)
	w.WriteString(notice)
	if err := c.sendPacket(protocol.PPartyUpdate, w.Bytes()); err != nil {
		return err
	}

	c.updatePartySyncCache(snapshot, leaderRID, snapshot.PendingInviteFrom, memberHashes)
	return nil
}

func (s *Server) sendPartySnapshotToCharacter(characterID string, noticeCode uint8, notice string) {
	client := s.findClientByCharacterID(characterID)
	if client == nil {
		return
	}
	if err := s.sendPartySnapshotToClient(client, noticeCode, notice); err != nil {
		// Non-fatal: packet queue can fail during disconnect races.
	}
}

func (s *Server) sendPartySnapshotToCharacters(characterIDs []string, noticeCode uint8, notice string) {
	seen := make(map[string]struct{}, len(characterIDs))
	for _, characterID := range characterIDs {
		id := strings.TrimSpace(characterID)
		if id == "" {
			continue
		}
		if _, exists := seen[id]; exists {
			continue
		}
		seen[id] = struct{}{}
		s.sendPartySnapshotToCharacter(id, noticeCode, notice)
	}
}

func (c *ClientConn) processPartyAction(action uint8, targetName string) {
	if c == nil || c.server == nil || c.server.party == nil || c.actor == nil || c.actor.IsDead() {
		return
	}

	selfCharacterID := c.actor.CharacterID
	selfName := c.actor.Name
	sendSelfNotice := func(code uint8, text string) {
		c.server.sendPartySnapshotToCharacter(selfCharacterID, code, text)
	}

	switch action {
	case protocol.PartyActionInvite:
		targetConn := c.server.findClientByName(targetName)
		if targetConn == nil || targetConn.actor == nil || targetConn.state != StateInGame {
			sendSelfNotice(protocol.PartyNoticeErrorTargetOffline, "Target player is not online.")
			return
		}
		if targetConn.actor.CharacterID == selfCharacterID {
			sendSelfNotice(protocol.PartyNoticeErrorCannotInviteSelf, "You cannot invite yourself.")
			return
		}
		if targetConn.actor.AreaName != c.actor.AreaName {
			sendSelfNotice(protocol.PartyNoticeErrorTargetArea, "Target must be in the same area.")
			return
		}
		_, err := c.server.party.invite(selfCharacterID, selfName, targetConn.actor.CharacterID)
		if err != nil {
			sendSelfNotice(partyErrorCode(err), partyErrorText(err))
			return
		}

		sendSelfNotice(protocol.PartyNoticeInviteSent, fmt.Sprintf("Party invite sent to %s.", targetConn.actor.Name))
		c.server.sendPartySnapshotToCharacter(
			targetConn.actor.CharacterID,
			protocol.PartyNoticeInviteReceived,
			fmt.Sprintf("%s invited you to a party.", selfName),
		)

	case protocol.PartyActionAccept:
		partyID, err := c.server.party.accept(selfCharacterID)
		if err != nil {
			sendSelfNotice(partyErrorCode(err), partyErrorText(err))
			return
		}
		members := c.server.party.partyMemberIDsLockedForRead(partyID)
		c.server.sendPartySnapshotToCharacters(members, protocol.PartyNoticeJoined, fmt.Sprintf("%s joined the party.", selfName))

	case protocol.PartyActionDecline:
		inviterCharacterID, err := c.server.party.decline(selfCharacterID)
		if err != nil {
			sendSelfNotice(partyErrorCode(err), partyErrorText(err))
			return
		}
		sendSelfNotice(protocol.PartyNoticeInviteDeclined, "Party invite declined.")
		if inviterCharacterID != "" {
			c.server.sendPartySnapshotToCharacter(
				inviterCharacterID,
				protocol.PartyNoticeInviteDeclined,
				fmt.Sprintf("%s declined your party invite.", selfName),
			)
		}

	case protocol.PartyActionLeave:
		_, remainingMembers, clearedInviteTargets, err := c.server.party.leave(selfCharacterID)
		if err != nil {
			sendSelfNotice(partyErrorCode(err), partyErrorText(err))
			return
		}
		sendSelfNotice(protocol.PartyNoticeLeft, "You left the party.")
		c.server.sendPartySnapshotToCharacters(remainingMembers, protocol.PartyNoticeLeft, fmt.Sprintf("%s left the party.", selfName))
		c.server.sendPartySnapshotToCharacters(clearedInviteTargets, protocol.PartyNoticeInviteCancelled, "Your pending party invite was cancelled.")

	case protocol.PartyActionKick:
		targetConn := c.server.findClientByName(targetName)
		if targetConn == nil || targetConn.actor == nil {
			sendSelfNotice(protocol.PartyNoticeErrorTargetOffline, "Target player is not online.")
			return
		}
		_, remainingMembers, err := c.server.party.kick(selfCharacterID, targetConn.actor.CharacterID)
		if err != nil {
			sendSelfNotice(partyErrorCode(err), partyErrorText(err))
			return
		}
		c.server.sendPartySnapshotToCharacter(
			targetConn.actor.CharacterID,
			protocol.PartyNoticeKicked,
			fmt.Sprintf("You were removed from the party by %s.", selfName),
		)
		c.server.sendPartySnapshotToCharacters(
			remainingMembers,
			protocol.PartyNoticeKicked,
			fmt.Sprintf("%s was removed from the party.", targetConn.actor.Name),
		)

	case protocol.PartyActionTransferLead:
		targetConn := c.server.findClientByName(targetName)
		if targetConn == nil || targetConn.actor == nil {
			sendSelfNotice(protocol.PartyNoticeErrorTargetOffline, "Target player is not online.")
			return
		}
		partyID, members, err := c.server.party.transferLeader(selfCharacterID, targetConn.actor.CharacterID)
		if err != nil {
			sendSelfNotice(partyErrorCode(err), partyErrorText(err))
			return
		}
		if len(members) == 0 {
			members = c.server.party.partyMemberIDsLockedForRead(partyID)
		}
		c.server.sendPartySnapshotToCharacters(
			members,
			protocol.PartyNoticeLeaderTransferred,
			fmt.Sprintf("%s is now the party leader.", targetConn.actor.Name),
		)

	default:
		sendSelfNotice(protocol.PartyNoticeErrorUnsupported, "Unsupported party action.")
	}
}

func (c *ClientConn) handlePartyDisconnect() {
	if c == nil || c.server == nil || c.server.party == nil || c.actor == nil {
		return
	}
	_, remainingMembers, clearedInviteTargets, err := c.server.party.leave(c.actor.CharacterID)
	if err != nil {
		return
	}
	notice := fmt.Sprintf("%s left the party.", c.actor.Name)
	c.server.sendPartySnapshotToCharacters(remainingMembers, protocol.PartyNoticeLeft, notice)
	c.server.sendPartySnapshotToCharacters(
		clearedInviteTargets,
		protocol.PartyNoticeInviteCancelled,
		"Your pending party invite was cancelled.",
	)
}
