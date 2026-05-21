package net

import (
	"fmt"
	"log"
	"time"

	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

const (
	combatDodgeSPCost         int32   = 18
	combatDodgeCooldownMs     int64   = 900
	combatDodgeIFrameMs       int64   = 280
	combatGuardSPCost         int32   = 12
	combatGuardCooldownMs     int64   = 650
	combatParrySPCost         int32   = 10
	combatParryCooldownMs     int64   = 1100
	combatParryWindowMs       int64   = 300
	combatInterruptCooldownMs int64   = 900
	combatInterruptRange      float32 = 4.0
)

func (c *ClientConn) sendCombatEventToSelf(eventCode uint8, sourceRID, targetRID uint32, value int16, text string) {
	if c == nil || c.actor == nil {
		return
	}
	var w Writer
	w.WriteUint8(eventCode)
	w.WriteUint32(sourceRID)
	w.WriteUint32(targetRID)
	w.WriteUint16(uint16(value))
	w.WriteString(text)
	c.actor.Send(buildFramedPacket(protocol.PCombatEvent, w.Bytes()))
}

func (c *ClientConn) broadcastCombatEvent(eventCode uint8, sourceRID, targetRID uint32, value int16, text string) {
	if c == nil || c.server == nil || c.actor == nil || c.server.world == nil {
		return
	}
	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if !ok || area == nil {
		c.sendCombatEventToSelf(eventCode, sourceRID, targetRID, value, text)
		return
	}
	world.BroadcastCombatEvent(area, eventCode, sourceRID, targetRID, value, text)
}

func (c *ClientConn) rejectCombatAction(action uint8, text string) {
	if c == nil || c.actor == nil {
		return
	}
	user := ""
	if c.account != nil {
		user = c.account.Username
	}
	log.Printf("ingame-action: combat REJECTED user=%s rid=%d action=%d reason=%q",
		user, c.actor.RuntimeID, action, text)
	c.sendCombatEventToSelf(protocol.CombatEventActionRejected, c.actor.RuntimeID, 0, int16(action), text)
}

func (c *ClientConn) combatActionAreaForAnimate() *world.Area {
	if c == nil || c.server == nil || c.server.world == nil || c.actor == nil {
		return nil
	}
	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if !ok || area == nil {
		log.Printf("WARN: combat action: area %q not found for actor=%d",
			c.actor.AreaName, c.actor.RuntimeID)
		return nil
	}
	return area
}

func (c *ClientConn) processCombatAction(action uint8, targetRID uint32) {
	if c == nil || c.server == nil || c.actor == nil || c.actor.IsDead() {
		return
	}
	now := time.Now().UnixMilli()

	switch action {
	case protocol.CombatActionDodge:
		c.actor.Mu.Lock()
		if now-c.actor.LastDodgeAt < combatDodgeCooldownMs {
			c.actor.Mu.Unlock()
			if area := c.combatActionAreaForAnimate(); area != nil {
				world.BroadcastAnimate(area, c.actor, "Idle")
			}
			c.rejectCombatAction(action, "Dodge is on cooldown.")
			return
		}
		if c.actor.Stamina < combatDodgeSPCost {
			c.actor.Mu.Unlock()
			if area := c.combatActionAreaForAnimate(); area != nil {
				world.BroadcastAnimate(area, c.actor, "Idle")
			}
			c.rejectCombatAction(action, "Not enough SP for dodge.")
			return
		}
		c.actor.Stamina -= combatDodgeSPCost
		c.actor.LastDodgeAt = now
		c.actor.DodgeUntil = now + combatDodgeIFrameMs
		c.actor.LastCombatAt = now
		sp := c.actor.Stamina
		c.actor.Mu.Unlock()

		if area := c.combatActionAreaForAnimate(); area != nil {
			world.BroadcastAnimate(area, c.actor, "Idle")
		}
		world.BroadcastSPUpdate(c.actor, sp)
		c.broadcastCombatEvent(
			protocol.CombatEventDodgeStarted,
			c.actor.RuntimeID,
			0,
			int16(combatDodgeIFrameMs),
			"Dodge activated.",
		)
		return

	case protocol.CombatActionGuardStart:
		c.actor.Mu.Lock()
		if now-c.actor.LastGuardAt < combatGuardCooldownMs {
			c.actor.Mu.Unlock()
			if area := c.combatActionAreaForAnimate(); area != nil {
				world.BroadcastAnimate(area, c.actor, "Idle")
			}
			c.rejectCombatAction(action, "Defense skill is on cooldown.")
			return
		}
		if c.actor.Stamina < combatGuardSPCost {
			c.actor.Mu.Unlock()
			if area := c.combatActionAreaForAnimate(); area != nil {
				world.BroadcastAnimate(area, c.actor, "Idle")
			}
			c.rejectCombatAction(action, "Not enough SP for defense skill.")
			return
		}
		c.actor.Stamina -= combatGuardSPCost
		c.actor.Guarding = true
		c.actor.GuardUntil = 0
		c.actor.LastGuardAt = now
		c.actor.LastCombatAt = now
		sp := c.actor.Stamina
		c.actor.Mu.Unlock()

		if area := c.combatActionAreaForAnimate(); area != nil {
			world.BroadcastAnimate(area, c.actor, "Idle")
		}
		world.BroadcastSPUpdate(c.actor, sp)
		c.broadcastCombatEvent(
			protocol.CombatEventGuardStarted,
			c.actor.RuntimeID,
			0,
			0,
			"Defense skill active.",
		)
		return

	case protocol.CombatActionGuardEnd:
		c.actor.Mu.Lock()
		if !c.actor.Guarding && c.actor.GuardUntil == 0 {
			c.actor.Mu.Unlock()
			return
		}
		c.actor.Guarding = false
		c.actor.GuardUntil = 0
		c.actor.Mu.Unlock()
		c.broadcastCombatEvent(protocol.CombatEventGuardEnded, c.actor.RuntimeID, 0, 0, "Guard down.")
		return

	case protocol.CombatActionParryStart:
		c.actor.Mu.Lock()
		if now-c.actor.LastParryAt < combatParryCooldownMs {
			c.actor.Mu.Unlock()
			if area := c.combatActionAreaForAnimate(); area != nil {
				world.BroadcastAnimate(area, c.actor, "Idle")
			}
			c.rejectCombatAction(action, "Parry is on cooldown.")
			return
		}
		if c.actor.Stamina < combatParrySPCost {
			c.actor.Mu.Unlock()
			if area := c.combatActionAreaForAnimate(); area != nil {
				world.BroadcastAnimate(area, c.actor, "Idle")
			}
			c.rejectCombatAction(action, "Not enough SP for parry.")
			return
		}
		c.actor.Stamina -= combatParrySPCost
		c.actor.LastParryAt = now
		c.actor.ParryUntil = now + combatParryWindowMs
		c.actor.LastCombatAt = now
		sp := c.actor.Stamina
		c.actor.Mu.Unlock()

		if area := c.combatActionAreaForAnimate(); area != nil {
			world.BroadcastAnimate(area, c.actor, "Idle")
		}
		world.BroadcastSPUpdate(c.actor, sp)
		c.broadcastCombatEvent(
			protocol.CombatEventParryStarted,
			c.actor.RuntimeID,
			0,
			int16(combatParryWindowMs),
			"Parry window open.",
		)
		return

	case protocol.CombatActionParryEnd:
		c.actor.Mu.Lock()
		if c.actor.ParryUntil == 0 {
			c.actor.Mu.Unlock()
			return
		}
		c.actor.ParryUntil = 0
		c.actor.Mu.Unlock()
		c.broadcastCombatEvent(protocol.CombatEventParryEnded, c.actor.RuntimeID, 0, 0, "Parry window closed.")
		return

	case protocol.CombatActionInterrupt:
		c.actor.Mu.Lock()
		if now-c.actor.LastInterruptAt < combatInterruptCooldownMs {
			c.actor.Mu.Unlock()
			c.rejectCombatAction(action, "Interrupt is on cooldown.")
			return
		}
		c.actor.LastInterruptAt = now
		c.actor.LastCombatAt = now
		c.actor.Mu.Unlock()

		target, area := c.server.world.FindActor(targetRID)
		if target == nil || area == nil || area.Name != c.actor.AreaName || target.IsDead() {
			c.rejectCombatAction(action, "Invalid interrupt target.")
			return
		}

		c.actor.Mu.Lock()
		sx, sz := c.actor.X, c.actor.Z
		c.actor.Mu.Unlock()
		target.Mu.Lock()
		tx, tz := target.X, target.Z
		target.Mu.Unlock()
		dx := sx - tx
		dz := sz - tz
		if dx*dx+dz*dz > combatInterruptRange*combatInterruptRange {
			c.rejectCombatAction(action, "Target is out of interrupt range.")
			return
		}

		target.Mu.Lock()
		targetWasDefending := target.Guarding ||
			target.ParryUntil > now || target.DodgeUntil > now
		if targetWasDefending {
			target.Guarding = false
			target.GuardUntil = 0
			target.ParryUntil = 0
			target.DodgeUntil = 0
		}
		target.Mu.Unlock()

		if !targetWasDefending {
			c.rejectCombatAction(action, "Target has no interruptible state.")
			return
		}

		c.broadcastCombatEvent(
			protocol.CombatEventInterruptSuccess,
			c.actor.RuntimeID,
			targetRID,
			0,
			fmt.Sprintf("%s interrupted %s.", c.actor.Name, target.Name),
		)
		return
	}

	c.rejectCombatAction(action, "Unsupported combat action.")
}
