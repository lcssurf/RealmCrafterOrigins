package net

import (
	"context"
	"log"
	"sort"
	"strings"

	"realm-crafter/server/internal/world"
)

const partyXPShareRadius = float32(80.0)

// awardXP applies kill XP to the killer or shares it with nearby party members.
// Party XP sharing rules (baseline):
//   - same party as killer
//   - online and in-game
//   - alive
//   - same area as the kill
//   - within partyXPShareRadius from kill position
//
// XP is split evenly among eligible recipients; any remainder goes to the killer.
func (c *ClientConn) awardXP(ctx context.Context, npcLevel int, killX, killZ float32) error {
	if c == nil || c.actor == nil {
		return nil
	}
	totalGain := world.XPForKill(npcLevel)
	if totalGain <= 0 {
		return nil
	}

	recipients := c.collectXPRecipients(killX, killZ)
	if len(recipients) == 0 {
		recipients = []*ClientConn{c}
	}

	shares := distributePartyXPGain(totalGain, recipients, strings.TrimSpace(c.actor.CharacterID))
	for _, recipient := range recipients {
		gain := shares[recipient]
		if gain <= 0 {
			continue
		}
		if err := recipient.awardXPGain(ctx, gain); err != nil {
			targetName := ""
			if recipient.actor != nil {
				targetName = recipient.actor.Name
			}
			log.Printf("party-xp: failed awarding gain=%d to %q: %v", gain, targetName, err)
		}
	}
	return nil
}

func (c *ClientConn) collectXPRecipients(killX, killZ float32) []*ClientConn {
	if c == nil || c.server == nil || c.actor == nil {
		return nil
	}
	killerCharacterID := strings.TrimSpace(c.actor.CharacterID)
	killerAreaName := strings.TrimSpace(c.actor.AreaName)
	if killerCharacterID == "" {
		return []*ClientConn{c}
	}
	if c.server.party == nil {
		return []*ClientConn{c}
	}

	snapshot := c.server.party.snapshotFor(killerCharacterID)
	if snapshot.PartyID == 0 || len(snapshot.MemberCharacterIDs) == 0 {
		return []*ClientConn{c}
	}

	maxDistanceSq := partyXPShareRadius * partyXPShareRadius
	eligibleByCharacter := make(map[string]*ClientConn)

	for _, memberCharacterID := range snapshot.MemberCharacterIDs {
		memberID := strings.TrimSpace(memberCharacterID)
		if memberID == "" {
			continue
		}
		memberConn := c.server.findClientByCharacterID(memberID)
		if memberConn == nil || memberConn.actor == nil || memberConn.state != StateInGame {
			continue
		}

		actor := memberConn.actor
		actor.Mu.Lock()
		areaName := strings.TrimSpace(actor.AreaName)
		ax, az := actor.X, actor.Z
		dead := actor.DeadAt > 0
		actor.Mu.Unlock()

		if dead || areaName != killerAreaName {
			continue
		}

		dx := ax - killX
		dz := az - killZ
		if dx*dx+dz*dz > maxDistanceSq {
			continue
		}

		eligibleByCharacter[memberID] = memberConn
	}

	// Killer should always get XP from their own kill, even if no one else is eligible.
	if _, ok := eligibleByCharacter[killerCharacterID]; !ok {
		eligibleByCharacter[killerCharacterID] = c
	}

	orderedIDs := make([]string, 0, len(eligibleByCharacter))
	for characterID := range eligibleByCharacter {
		orderedIDs = append(orderedIDs, characterID)
	}
	sort.SliceStable(orderedIDs, func(i, j int) bool {
		if orderedIDs[i] == killerCharacterID {
			return true
		}
		if orderedIDs[j] == killerCharacterID {
			return false
		}
		return orderedIDs[i] < orderedIDs[j]
	})

	out := make([]*ClientConn, 0, len(orderedIDs))
	for _, characterID := range orderedIDs {
		out = append(out, eligibleByCharacter[characterID])
	}
	return out
}

func distributePartyXPGain(total int64, recipients []*ClientConn, killerCharacterID string) map[*ClientConn]int64 {
	out := make(map[*ClientConn]int64, len(recipients))
	if total <= 0 || len(recipients) == 0 {
		return out
	}

	share := total / int64(len(recipients))
	remainder := total % int64(len(recipients))

	var killer *ClientConn
	for _, recipient := range recipients {
		if recipient == nil {
			continue
		}
		out[recipient] = share
		if killer == nil && recipient.actor != nil && strings.TrimSpace(recipient.actor.CharacterID) == killerCharacterID {
			killer = recipient
		}
	}
	if killer == nil {
		killer = recipients[0]
	}
	out[killer] += remainder
	return out
}

func (c *ClientConn) awardXPGain(ctx context.Context, gain int64) error {
	if gain <= 0 || c == nil || c.server == nil || c.server.db == nil || c.actor == nil || strings.TrimSpace(c.actor.CharacterID) == "" {
		return nil
	}

	c.actor.Mu.Lock()
	curXP := c.actor.XP
	curLevel := int(c.actor.Level)
	c.actor.Mu.Unlock()

	newXP, newLevel, leveled := world.ProcessXPCumulative(curXP, curLevel, gain)
	if err := c.server.db.SaveXP(ctx, c.actor.CharacterID, newXP, newLevel); err != nil {
		log.Printf("client: save xp: %v", err)
	}

	var (
		newUnspent   int32
		primaryAfter world.PrimaryStats
	)
	c.actor.Mu.Lock()
	c.actor.XP = newXP
	c.actor.Level = uint16(newLevel)
	if leveled {
		cfg := world.GetCachedCharProgressionConfig()
		levelsGained := newLevel - curLevel
		if levelsGained > 0 {
			c.actor.UnspentStatPoints += int32(cfg.StatPointsPerLevel) * int32(levelsGained)
		}
		newUnspent = c.actor.UnspentStatPoints
		primaryAfter = c.actor.Primary
	}
	c.actor.Mu.Unlock()
	if leveled {
		c.actor.SetPrimaryStats(primaryAfter)
		world.RecomputeDerivedStats(c.actor)
		c.actor.Mu.Lock()
		c.actor.Health = c.actor.HealthMax
		c.actor.Energy = c.actor.EnergyMax
		c.actor.Mu.Unlock()
		if err := c.server.db.UpdateCharacterUnspentStatPoints(ctx, c.actor.CharacterID, newUnspent); err != nil {
			log.Printf("party-xp: persist unspent points failed char=%s: %v", c.actor.CharacterID, err)
		}
		c.sendStatPointsUpdate(newUnspent)
	}

	return c.sendXPUpdate()
}
