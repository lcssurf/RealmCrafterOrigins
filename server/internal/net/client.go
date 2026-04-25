package net

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"time"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/scripting"
	"realm-crafter/server/internal/world"

	"github.com/quic-go/quic-go"
)

// Connection state constants.
const (
	StateConnected     = 0
	StateAuthenticated = 1
	StateInGame        = 2
)

// ClientConn manages a single player's QUIC connection through a state machine:
//
//	StateConnected → StateAuthenticated → StateInGame
type ClientConn struct {
	conn            quic.Connection
	stream          quic.Stream
	server          *Server
	state           int
	account         *db.Account
	actor           *world.Actor
	activeDialogNPC uint32 // RID of NPC currently in dialog with this player; 0 = none
}

// Run is the main goroutine for a connected client.
func (c *ClientConn) Run(ctx context.Context) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("client: panic recovered: %v", r)
		}
		c.cleanup(ctx)
	}()

	// Accept the single bidirectional stream the client opens.
	stream, err := c.conn.AcceptStream(ctx)
	if err != nil {
		log.Printf("client: accept stream: %v", err)
		return
	}
	c.stream = stream

	// Initialise a dummy actor so the write loop can start before login.
	c.actor = world.NewActor()

	// Write loop: pump actor.SendCh → stream.
	go c.writeLoop()

	log.Printf("client: new connection from %s", c.conn.RemoteAddr())

	// Read loop.
	for {
		pktType, payload, err := ReadPacket(c.stream)
		if err != nil {
			if errors.Is(err, io.EOF) || isClosedErr(err) {
				return
			}
			log.Printf("client: read packet: %v", err)
			return
		}

		if err := c.dispatch(ctx, pktType, payload); err != nil {
			log.Printf("client: dispatch pkt=%d: %v", pktType, err)
			return
		}
	}
}

// writeLoop forwards packets from the actor's send channel to the QUIC stream.
func (c *ClientConn) writeLoop() {
	for {
		select {
		case data, ok := <-c.actor.SendCh:
			if !ok {
				return
			}
			if _, err := c.stream.Write(data); err != nil {
				if !isClosedErr(err) {
					log.Printf("client: write: %v", err)
				}
				return
			}
		case <-c.actor.Done():
			return
		}
	}
}

// dispatch routes an incoming packet to the correct handler based on state.
func (c *ClientConn) dispatch(ctx context.Context, pktType uint16, payload []byte) error {
	switch c.state {
	case StateConnected:
		switch pktType {
		case protocol.PCreateAccount:
			return c.handleCreateAccount(ctx, payload)
		case protocol.PVerifyAccount:
			return c.handleVerifyAccount(ctx, payload)
		case protocol.PPing:
			return c.sendPong()
		default:
			log.Printf("client: state=connected: unexpected packet %d", pktType)
		}

	case StateAuthenticated:
		switch pktType {
		case protocol.PFetchCharacter:
			return c.handleFetchCharacter(ctx, payload)
		case protocol.PCreateCharacter:
			return c.handleCreateCharacter(ctx, payload)
		case protocol.PDeleteCharacter:
			return c.handleDeleteCharacter(ctx, payload)
		case protocol.PStartGame:
			return c.handleStartGame(ctx, payload)
		case protocol.PPing:
			return c.sendPong()
		default:
			log.Printf("client: state=authenticated: unexpected packet %d", pktType)
		}

	case StateInGame:
		switch pktType {
		case protocol.PStandardUpdate:
			return c.handleStandardUpdate(ctx, payload)
		case protocol.PChatMessage:
			return c.handleChatMessage(ctx, payload)
		case protocol.PInventorySwap:
			return c.handleInventorySwap(ctx, payload)
		case protocol.PAttackActor:
			return c.handleAttackActor(ctx, payload)
		case protocol.PUseItem:
			return c.handleUseItem(ctx, payload)
		case protocol.PRespawnPlayer:
			return c.handleRespawnPlayer(ctx)
		case protocol.PCastSpell:
			return c.handleCastSpell(ctx, payload)
		case protocol.PRightClick:
			return c.handleRightClick(ctx, payload)
		case protocol.PDialogChoice:
			return c.handleDialogChoice(ctx, payload)
		case protocol.PPickupItem:
			return c.handlePickupItem(ctx, payload)
		case protocol.PShopAction:
			return c.handleShopAction(ctx, payload)
		case protocol.PPing:
			return c.sendPong()
		default:
			log.Printf("client: state=ingame: unexpected packet %d", pktType)
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// Connected-state handlers
// ---------------------------------------------------------------------------

func (c *ClientConn) handleCreateAccount(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	username, err := r.ReadString()
	if err != nil {
		return fmt.Errorf("handleCreateAccount: %w", err)
	}
	password, err := r.ReadString()
	if err != nil {
		return fmt.Errorf("handleCreateAccount: %w", err)
	}
	email, err := r.ReadString()
	if err != nil {
		return fmt.Errorf("handleCreateAccount: %w", err)
	}

	result, internalErr := c.server.accounts.Register(ctx, username, password, email)
	if internalErr != nil {
		log.Printf("client: register internal error: %v", internalErr)
	}

	msg := resultMessage(result)
	return c.sendLoginResult(result, msg)
}

func (c *ClientConn) handleVerifyAccount(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	username, err := r.ReadString()
	if err != nil {
		return fmt.Errorf("handleVerifyAccount: %w", err)
	}
	password, err := r.ReadString()
	if err != nil {
		return fmt.Errorf("handleVerifyAccount: %w", err)
	}

	account, result, internalErr := c.server.accounts.Login(ctx, username, password)
	if internalErr != nil {
		log.Printf("client: login internal error: %v", internalErr)
	}

	if result == protocol.ResultOK {
		c.account = account
		c.state = StateAuthenticated
		c.server.accounts.SetOnline(account.Username)
		log.Printf("client: %s authenticated", account.Username)
	}

	msg := resultMessage(result)
	return c.sendLoginResult(result, msg)
}

// ---------------------------------------------------------------------------
// Authenticated-state handlers
// ---------------------------------------------------------------------------

func (c *ClientConn) handleFetchCharacter(ctx context.Context, _ []byte) error {
	// Send the list of playable actor defs first so the client can populate
	// its character-creation dropdown before rendering the char select screen.
	if defs, err := c.server.db.ListPlayableDefs(ctx); err == nil {
		log.Printf("client: sending %d playable def(s) to %s", len(defs), c.account.Username)
		var wd Writer
		wd.WriteUint8(uint8(len(defs)))
		for _, d := range defs {
			wd.WriteUint16(uint16(d.ID))
			wd.WriteString(d.Name)
			wd.WriteString(d.DefaultRace)
			wd.WriteString(d.DefaultClass)
		}
		if sendErr := c.sendPacket(protocol.PPlayableDefs, wd.Bytes()); sendErr != nil {
			return sendErr
		}
	} else {
		log.Printf("client: ListPlayableDefs error: %v", err)
	}

	chars, err := c.server.db.ListCharacters(ctx, c.account.ID)
	if err != nil {
		log.Printf("client: list characters: %v", err)
		chars = nil
	}

	var w Writer
	w.WriteUint8(uint8(len(chars)))
	for _, ch := range chars {
		w.WriteUint8(uint8(ch.Slot))
		w.WriteString(ch.Name)
		w.WriteString(ch.Race)
		w.WriteString(ch.Class)
		w.WriteUint16(uint16(ch.Level))
		w.WriteString(ch.AreaName)
		w.WriteInt32(ch.Health)
		w.WriteInt32(ch.HealthMax)
		w.WriteUint16(uint16(ch.ActorDefID))
	}
	return c.sendPacket(protocol.PCharListResult, w.Bytes())
}

func (c *ClientConn) handleCreateCharacter(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	slot, err := r.ReadUint8()
	if err != nil {
		return err
	}
	name, err := r.ReadString()
	if err != nil {
		return err
	}
	actorDefID16, err := r.ReadUint16()
	if err != nil {
		return err
	}
	gender, err := r.ReadUint8()
	if err != nil {
		return err
	}

	if slot > 8 {
		return c.sendCreateCharResult(protocol.ResultInvalidName, "Invalid slot")
	}

	actorDefID := int(actorDefID16)

	// Resolve race/class from the actor def so they're stored correctly.
	race, class := "Unknown", "Unknown"
	if actorDefID > 0 {
		if def, defErr := c.server.db.LoadActorDef(ctx, actorDefID); defErr == nil && def != nil {
			if def.DefaultRace != "" {
				race = def.DefaultRace
			}
			if def.DefaultClass != "" {
				class = def.DefaultClass
			}
		}
	}

	_, createErr := c.server.db.CreateCharacter(ctx, c.account.ID, int(slot), name, race, class,
		int(gender), 0, 0, 0, 0, actorDefID)
	if createErr != nil {
		log.Printf("client: create character: %v", createErr)
		return c.sendCreateCharResult(protocol.ResultCharExists, "Name already taken")
	}

	log.Printf("client: %s created character %q slot=%d actor_def_id=%d", c.account.Username, name, slot, actorDefID)
	return c.sendCreateCharResult(protocol.ResultOK, "Character created")
}

func (c *ClientConn) handleDeleteCharacter(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	slot, err := r.ReadUint8()
	if err != nil {
		return err
	}

	if deleteErr := c.server.db.DeleteCharacter(ctx, c.account.ID, int(slot)); deleteErr != nil {
		log.Printf("client: delete character slot=%d: %v", slot, deleteErr)
		return c.sendDeleteCharResult(protocol.ResultInvalidName, "Character not found")
	}

	log.Printf("client: %s deleted character in slot=%d", c.account.Username, slot)
	return c.sendDeleteCharResult(protocol.ResultOK, "Character deleted")
}

func (c *ClientConn) handleStartGame(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	slot, err := r.ReadUint8()
	if err != nil {
		return err
	}

	char, err := c.server.db.GetCharacterBySlot(ctx, c.account.ID, int(slot))
	if err != nil {
		log.Printf("client: start game: get char slot=%d: %v", slot, err)
		return c.sendKick("Character not found")
	}

	// Build Actor.
	actor := world.NewActor()
	actor.RuntimeID = c.server.world.NextRuntimeID()
	actor.CharacterID = char.ID
	actor.AccountID = c.account.ID
	actor.Username = c.account.Username
	actor.Name = char.Name
	actor.Race = char.Race
	actor.Class = char.Class
	actor.Level = uint16(char.Level)
	actor.X = char.X
	actor.Y = char.Y
	actor.Z = char.Z
	actor.Yaw = char.Yaw
	actor.AreaName = char.AreaName
	actor.Health    = char.Health
	actor.HealthMax = char.HealthMax
	actor.Energy    = char.Energy
	actor.EnergyMax = char.EnergyMax
	actor.XP        = char.XP
	actor.Gold      = char.Gold
	actor.Strength  = int32(char.Level) * 3
	actor.Radius    = 0.4
	if wdmg, armor, err := c.server.db.GetEquippedStats(ctx, char.ID); err == nil {
		actor.WeaponDamage = wdmg
		actor.CachedArmor  = armor
	}
	actor.SpellCooldowns = make(map[uint16]int64)

	// Resolve player appearance from the actor def (same path as NPCs).
	log.Printf("client: handleStartGame actor_def_id=%d for char %q", char.ActorDefID, char.Name)
	if char.ActorDefID > 0 {
		if app := BuildAppearance(ctx, c.server.db, char.ActorDefID); app != nil {
			actor.Appearance = app
			log.Printf("client: appearance resolved: %d mesh(es)", len(app.Meshes))
		} else {
			log.Printf("client: BuildAppearance returned nil for actor_def_id=%d", char.ActorDefID)
		}
	}

	// Close the dummy actor created at connect time and swap in the real one.
	c.actor.Close()
	c.actor = actor

	// Restart the write loop with the new actor's channels.
	go c.writeLoop()

	area := c.server.world.GetOrCreateArea(char.AreaName)

	// Snapshot existing actors before adding the new one so we don't send
	// PNewActor to the entering player about themselves.
	existing := area.Snapshot()

	area.AddActor(actor)
	c.state = StateInGame

	log.Printf("client: %s entered world as %q (runtimeID=%d, area=%s)",
		c.account.Username, char.Name, actor.RuntimeID, char.AreaName)

	// Send PStartGame to the entering player.
	// Appearance section is appended after the base fields so the client can
	// initialize the player model before the first render frame.
	{
		var w Writer
		w.WriteUint32(actor.RuntimeID)
		w.WriteString(actor.AreaName)
		w.WriteFloat32(actor.X)
		w.WriteFloat32(actor.Y)
		w.WriteFloat32(actor.Z)
		w.WriteFloat32(actor.Yaw)
		w.WriteInt32(actor.Health)
		w.WriteInt32(actor.HealthMax)
		w.WriteInt32(actor.Energy)
		w.WriteInt32(actor.EnergyMax)
		payload := append(w.Bytes(), world.AppearanceBytes(actor.Appearance)...)
		if err := c.sendPacket(protocol.PStartGame, payload); err != nil {
			return err
		}
	}

	// Send initial XP/level state.
	_ = c.sendXPUpdate()

	// Give starter items if this is a new character, then send inventory.
	_ = c.server.db.GiveStarterItems(ctx, char.ID)
	_ = c.sendInventory(ctx, char.ID)

	// Inform the entering player about everyone already in the zone.
	for _, other := range existing {
		pkt := world.NewActorPayload(other)
		if err := c.sendPacket(protocol.PNewActor, pkt); err != nil {
			return err
		}
	}

	// Inform everyone already in the zone about the new player.
	newActorPkt := world.NewActorPayload(actor)
	area.Broadcast(buildFramedPacket(protocol.PNewActor, newActorPkt), actor.RuntimeID)

	// Also send the player's own PNewActor to themselves so the client can
	// resolve the actor def appearance (model path, animations, etc.).
	if err := c.sendPacket(protocol.PNewActor, newActorPkt); err != nil {
		return err
	}

	// Send portal positions so the client can render markers.
	c.sendPortals(area)

	// Send any dropped items already in the area.
	c.sendWorldItems(area)

	// Send gold balance.
	c.sendGoldUpdate()

	// Send known spells.
	c.sendKnownSpells()

	// Start area music.
	c.sendMusic(musicForArea(actor.AreaName), 128)

	return nil
}

// ---------------------------------------------------------------------------
// In-game handlers
// ---------------------------------------------------------------------------

func (c *ClientConn) handleStandardUpdate(_ context.Context, payload []byte) error {
	r := NewReader(payload)
	x, err := r.ReadFloat32()
	if err != nil {
		return err
	}
	y, err := r.ReadFloat32()
	if err != nil {
		return err
	}
	z, err := r.ReadFloat32()
	if err != nil {
		return err
	}
	yaw, err := r.ReadFloat32()
	if err != nil {
		return err
	}
	flags, err := r.ReadUint8()
	if err != nil {
		return err
	}

	c.actor.X = x
	c.actor.Y = y
	c.actor.Z = z
	c.actor.Yaw = yaw

	var w Writer
	w.WriteUint32(c.actor.RuntimeID)
	w.WriteFloat32(x)
	w.WriteFloat32(y)
	w.WriteFloat32(z)
	w.WriteFloat32(yaw)
	w.WriteUint8(flags)

	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if ok {
		area.Broadcast(buildFramedPacket(protocol.PStandardUpdate, w.Bytes()), c.actor.RuntimeID)

		// Check portal triggers.
		if portal := c.server.world.CheckPortal(c.actor, area); portal != nil {
			return c.triggerPortal(area, portal)
		}
	}
	return nil
}

func (c *ClientConn) sendPortals(area *world.Area) {
	area.Mu.RLock()
	portals := area.Portals
	area.Mu.RUnlock()

	var w Writer
	w.WriteUint8(uint8(len(portals)))
	for _, p := range portals {
		w.WriteFloat32(p.X)
		w.WriteFloat32(p.Z)
		w.WriteFloat32(p.Radius)
		w.WriteString(p.TargetArea)
	}
	c.actor.Send(buildFramedPacket(protocol.PPortalInfo, w.Bytes()))
}

func (c *ClientConn) triggerPortal(oldArea *world.Area, portal *world.Portal) error {
	// Cooldown: 3 s between portal uses.
	c.actor.Mu.Lock()
	now := time.Now().UnixMilli()
	if now-c.actor.LastPortal < 3000 {
		c.actor.Mu.Unlock()
		return nil
	}
	c.actor.LastPortal = now
	c.actor.Mu.Unlock()

	// Remove from old area and tell everyone there the player left.
	oldArea.RemoveActor(c.actor.RuntimeID)
	var gone Writer
	gone.WriteUint32(c.actor.RuntimeID)
	oldArea.BroadcastAll(buildFramedPacket(protocol.PActorGone, gone.Bytes()))

	// Move actor to destination.
	c.actor.X = portal.DestX
	c.actor.Y = portal.DestY
	c.actor.Z = portal.DestZ
	c.actor.Yaw = portal.DestYaw
	c.actor.AreaName = portal.TargetArea

	// Add to new area.
	newArea := c.server.world.GetOrCreateArea(portal.TargetArea)
	existing := newArea.Snapshot()
	newArea.AddActor(c.actor)

	// Tell the client to change area.
	var ca Writer
	ca.WriteString(portal.TargetArea)
	ca.WriteFloat32(portal.DestX)
	ca.WriteFloat32(portal.DestY)
	ca.WriteFloat32(portal.DestZ)
	ca.WriteFloat32(portal.DestYaw)
	c.actor.Send(buildFramedPacket(protocol.PChangeArea, ca.Bytes()))

	// Send all existing actors in the new area to the client.
	for _, other := range existing {
		c.actor.Send(buildFramedPacket(protocol.PNewActor, world.NewActorPayload(other)))
	}

	// Tell everyone in the new area about the arriving player.
	newArea.Broadcast(buildFramedPacket(protocol.PNewActor, world.NewActorPayload(c.actor)), c.actor.RuntimeID)

	// Send portal positions and dropped items in the new area.
	c.sendPortals(newArea)
	c.sendWorldItems(newArea)

	// Resend known spells — the client clears its spellbar on PChangeArea,
	// so without this the bar stays empty after a portal.
	c.sendKnownSpells()

	// Portal FX and area music for the arriving player.
	c.sendSound(protocol.SoundPortal, 220)
	c.broadcastEmitter(newArea, protocol.EmitterPortal, portal.DestX, portal.DestY, portal.DestZ, 2000)
	c.sendMusic(musicForArea(portal.TargetArea), 128)

	log.Printf("client: %s portal → %s (%.0f,%.0f,%.0f)",
		c.actor.Name, portal.TargetArea, portal.DestX, portal.DestY, portal.DestZ)
	return nil
}

func (c *ClientConn) handleChatMessage(_ context.Context, payload []byte) error {
	r := NewReader(payload)
	channel, err := r.ReadUint8()
	if err != nil {
		return err
	}
	message, err := r.ReadString()
	if err != nil {
		return err
	}

	var w Writer
	w.WriteUint8(channel)
	w.WriteString(c.actor.Name)
	w.WriteString(message)

	framed := buildFramedPacket(protocol.PChatMessage, w.Bytes())

	// Deliver to self.
	c.actor.Send(framed)

	// Broadcast to zone (broadcast already excludes self, so we deliver
	// separately above — pass 0 as exceptID is safe but would double-send,
	// so use actor.RuntimeID and then send directly to self).
	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if ok {
		area.Broadcast(framed, c.actor.RuntimeID)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Ping / Pong
// ---------------------------------------------------------------------------

func (c *ClientConn) sendPong() error {
	return c.sendPacket(protocol.PPong, nil)
}

func (c *ClientConn) handleAttackActor(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	targetRID, err := r.ReadUint32()
	if err != nil {
		return err
	}

	target, area := c.server.world.FindActor(targetRID)
	if target == nil || area == nil || area.Name != c.actor.AreaName {
		return nil
	}

	// Don't attack dead things.
	if target.IsDead() {
		return nil
	}

	// Distance check.
	if !world.InMeleeRange(c.actor, target) {
		return nil
	}

	// Make defensive/aggressive NPC fight back.
	if target.IsNPC {
		target.Mu.Lock()
		if target.AIMode == world.AIWait &&
			(target.Aggressiveness == 1 || target.Aggressiveness == 2) {
			target.AIMode = world.AIChase
			target.AITarget = c.actor
		}
		target.Mu.Unlock()
	}

	dmg, isCrit, onCD := world.ProcessAttack(c.actor, target)
	if onCD {
		return nil
	}

	died := world.BroadcastAttack(area, c.actor, target, dmg, isCrit)
	if died && target.IsNPC {
		x, y, z := target.X, target.Y, target.Z
		area.KillNPC(target)
		area.SpawnDropsForNPC(target)
		c.broadcastEmitter(area, protocol.EmitterExplosion, x, y, z, 0)
		c.broadcastSound(area, protocol.SoundNPCDeath, 200)
		return c.awardXP(ctx, int(target.Level))
	}
	return nil
}

func (c *ClientConn) awardXP(ctx context.Context, npcLevel int) error {
	gain := world.XPForKill(npcLevel)

	c.actor.Mu.Lock()
	curXP := c.actor.XP
	curLevel := int(c.actor.Level)
	c.actor.Mu.Unlock()

	newXP, newLevel, leveled := world.ProcessXP(curXP, curLevel, gain)
	hpMax, epMax, strength := world.StatsByLevel(newLevel)

	if err := c.server.db.SaveXP(ctx, c.actor.CharacterID, newXP, newLevel, hpMax, epMax); err != nil {
		log.Printf("client: save xp: %v", err)
	}

	c.actor.Mu.Lock()
	c.actor.XP = newXP
	c.actor.Level = uint16(newLevel)
	if leveled {
		c.actor.HealthMax = hpMax
		c.actor.EnergyMax = epMax
		c.actor.Health = hpMax
		c.actor.Energy = epMax // full restore on level up
		c.actor.Strength = strength
	}
	c.actor.Mu.Unlock()

	return c.sendXPUpdate()
}

func (c *ClientConn) sendXPUpdate() error {
	c.actor.Mu.Lock()
	level := c.actor.Level
	xp := c.actor.XP
	hp := c.actor.Health
	hpMax := c.actor.HealthMax
	c.actor.Mu.Unlock()

	xpNext := world.XPToLevel(int(level) + 1)

	var w Writer
	w.WriteUint16(level)
	w.WriteUint32(uint32(xp))
	w.WriteUint32(uint32(xpNext))
	if err := c.sendPacket(protocol.PXPUpdate, w.Bytes()); err != nil {
		return err
	}

	// Push HP, healthMax and energyMax so the HUD stays in sync.
	rid := c.actor.RuntimeID
	c.actor.Mu.Lock()
	ep    := c.actor.Energy
	epMax := c.actor.EnergyMax
	c.actor.Mu.Unlock()

	for _, attr := range []struct{ id uint8; val int32 }{
		{1, hpMax},
		{0, hp},
		{3, epMax},
		{2, ep},
	} {
		var sp Writer
		sp.WriteUint8('A')
		sp.WriteUint32(rid)
		sp.WriteUint8(attr.id)
		sp.WriteUint16(uint16(int16(attr.val)))
		c.actor.Send(buildFramedPacket(protocol.PStatUpdate, sp.Bytes()))
	}

	return nil
}

// sendInventory fetches all items for charID and sends PInventoryUpdate.
func (c *ClientConn) sendInventory(ctx context.Context, charID string) error {
	items, err := c.server.db.GetInventory(ctx, charID)
	if err != nil {
		return err
	}
	var w Writer
	w.WriteUint8(uint8(len(items)))
	for _, ci := range items {
		w.WriteUint8(ci.Slot)
		w.WriteUint16(ci.ItemID)
		w.WriteUint8(ci.Quantity)
		w.WriteUint8(ci.Durability)
		w.WriteString(ci.Name)
		w.WriteUint8(ci.ItemType)
		w.WriteUint8(ci.SlotType)
		w.WriteUint16(uint16(ci.WeaponDamage))
		w.WriteUint16(uint16(ci.ArmorLevel))
	}
	return c.sendPacket(protocol.PInventoryUpdate, w.Bytes())
}

func (c *ClientConn) handleInventorySwap(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	slotA, err := r.ReadUint8()
	if err != nil {
		return err
	}
	slotB, err := r.ReadUint8()
	if err != nil {
		return err
	}

	if err := c.server.db.SwapInventorySlots(ctx, c.actor.CharacterID, slotA, slotB); err != nil {
		log.Printf("client: inventory swap %d↔%d: %v", slotA, slotB, err)
	}
	// Refresh equipped combat stats after any equip change.
	if wdmg, armor, err := c.server.db.GetEquippedStats(ctx, c.actor.CharacterID); err == nil {
		c.actor.WeaponDamage = wdmg
		c.actor.CachedArmor  = armor
	}
	return c.sendInventory(ctx, c.actor.CharacterID)
}

func (c *ClientConn) handleUseItem(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	slot, err := r.ReadUint8()
	if err != nil {
		return err
	}

	res, err := c.server.db.UseItem(ctx, c.actor.CharacterID, slot)
	if err != nil {
		log.Printf("client: use item slot %d: %v", slot, err)
		return nil // not fatal — just ignore
	}

	// Consumable: apply heal and broadcast new HP.
	if res.ItemType == 2 && res.HealAmt > 0 {
		c.actor.Mu.Lock()
		newHP := c.actor.Health + res.HealAmt
		if newHP > c.actor.HealthMax {
			newHP = c.actor.HealthMax
		}
		c.actor.Health = newHP
		hp := newHP
		c.actor.Mu.Unlock()

		var sw Writer
		sw.WriteUint8('A')
		sw.WriteUint32(c.actor.RuntimeID)
		sw.WriteUint8(0) // attr 0 = HP
		sw.WriteUint16(uint16(int16(hp)))
		c.actor.Send(buildFramedPacket(protocol.PStatUpdate, sw.Bytes()))
	}

	// Equip change: refresh cached combat stats.
	if res.EquipSlot != 0xFF {
		if wdmg, armor, err := c.server.db.GetEquippedStats(ctx, c.actor.CharacterID); err == nil {
			c.actor.WeaponDamage = wdmg
			c.actor.CachedArmor = armor
		}
	}

	return c.sendInventory(ctx, c.actor.CharacterID)
}

func (c *ClientConn) handleRespawnPlayer(ctx context.Context) error {
	c.actor.Mu.Lock()
	if c.actor.Health > 0 {
		c.actor.Mu.Unlock()
		return nil // not dead — ignore
	}
	c.actor.Health = c.actor.HealthMax
	c.actor.DeadAt = 0
	c.actor.X = c.actor.SpawnX
	c.actor.Y = c.actor.SpawnY
	c.actor.Z = c.actor.SpawnZ
	c.actor.Yaw = c.actor.SpawnYaw
	hp := c.actor.Health
	x, y, z, yaw := c.actor.X, c.actor.Y, c.actor.Z, c.actor.Yaw
	rid := c.actor.RuntimeID
	c.actor.Mu.Unlock()

	// Teleport the client to spawn position.
	var rp Writer
	rp.WriteUint32(rid)
	rp.WriteFloat32(x)
	rp.WriteFloat32(y)
	rp.WriteFloat32(z)
	rp.WriteFloat32(yaw)
	c.actor.Send(buildFramedPacket(protocol.PRepositionActor, rp.Bytes()))

	// Restore HP on client HUD.
	var sp Writer
	sp.WriteUint8('A')
	sp.WriteUint32(rid)
	sp.WriteUint8(0)
	sp.WriteUint16(uint16(int16(hp)))
	c.actor.Send(buildFramedPacket(protocol.PStatUpdate, sp.Bytes()))

	// Tell other players about the new position.
	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if ok {
		var up Writer
		up.WriteUint32(rid)
		up.WriteFloat32(x)
		up.WriteFloat32(y)
		up.WriteFloat32(z)
		up.WriteFloat32(yaw)
		up.WriteUint8(0)
		area.Broadcast(buildFramedPacket(protocol.PStandardUpdate, up.Bytes()), rid)
	}

	return nil
}

// ---------------------------------------------------------------------------
// Spell handlers
// ---------------------------------------------------------------------------

func (c *ClientConn) sendKnownSpells() {
	spells := c.server.scripting.AllSpells()
	var w Writer
	w.WriteUint8(uint8(len(spells)))
	for _, def := range spells {
		w.WriteUint16(def.ID)
		w.WriteString(def.Name)
		w.WriteUint8(scripting.SpellTypeIndex(def.SpellType))
		w.WriteUint16(uint16(def.EPCost))
		w.WriteUint32(uint32(def.CooldownMs))
		w.WriteFloat32(def.Range)
		w.WriteUint8(def.Icon)
		w.WriteUint8(def.AoEType)
		w.WriteFloat32(def.AoERadius)
	}
	c.actor.Send(buildFramedPacket(protocol.PKnownSpells, w.Bytes()))
}

func (c *ClientConn) handleCastSpell(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	spellID, err := r.ReadUint16()
	if err != nil {
		return err
	}
	targetRID, err := r.ReadUint32()
	if err != nil {
		return err
	}
	// ground_x/z: valid for aoe_type == 2; always sent by client (0 otherwise).
	groundX, _ := r.ReadFloat32()
	groundZ, _ := r.ReadFloat32()

	if c.actor.IsDead() {
		return nil
	}

	def := c.server.scripting.GetSpell(spellID)
	if def == nil {
		log.Printf("client: %s cast unknown spell %d", c.actor.Name, spellID)
		return nil
	}

	// Validate and deduct EP + cooldown under lock.
	c.actor.Mu.Lock()
	now := time.Now().UnixMilli()
	if now-c.actor.SpellCooldowns[spellID] < def.CooldownMs {
		c.actor.Mu.Unlock()
		return nil
	}
	if c.actor.Energy < def.EPCost {
		c.actor.Mu.Unlock()
		return nil
	}
	c.actor.Energy -= def.EPCost
	c.actor.SpellCooldowns[spellID] = now
	c.actor.LastCombatAt = now
	ep := c.actor.Energy
	c.actor.Mu.Unlock()

	// Broadcast EP update immediately.
	{
		var sw Writer
		sw.WriteUint8('A')
		sw.WriteUint32(c.actor.RuntimeID)
		sw.WriteUint8(2)
		sw.WriteUint16(uint16(int16(ep)))
		c.actor.Send(buildFramedPacket(protocol.PStatUpdate, sw.Bytes()))
	}

	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if !ok {
		return nil
	}

	var target *world.Actor
	needsTarget := def.SpellType == "damage" || def.SpellType == "debuff"
	isGroundAoE := def.AoEType == 2

	if needsTarget && !isGroundAoE {
		target, _ = area.GetActor(targetRID)
		if target == nil || target.IsDead() {
			return nil
		}
		dx := c.actor.X - target.X
		dz := c.actor.Z - target.Z
		if dx*dx+dz*dz > def.Range*def.Range {
			return nil
		}
		// Provoke defensive/aggressive NPC.
		if target.IsNPC {
			target.Mu.Lock()
			if target.AIMode == world.AIWait &&
				(target.Aggressiveness == 1 || target.Aggressiveness == 2) {
				target.AIMode = world.AIChase
				target.AITarget = c.actor
			}
			target.Mu.Unlock()
		}
	} else if isGroundAoE {
		// Validate ground position is within range.
		dx := c.actor.X - groundX
		dz := c.actor.Z - groundZ
		if dx*dx+dz*dz > def.Range*def.Range {
			return nil
		}
	}

	log.Printf("client: %s casting %q (id=%d) target=%d ground=(%.1f,%.1f)",
		c.actor.Name, def.Name, def.ID, targetRID, groundX, groundZ)
	killedRID := c.server.scripting.Cast(def, c.actor, target, area, groundX, groundZ)
	if killedRID != 0 {
		if killed, _ := area.GetActor(killedRID); killed != nil && killed.IsNPC {
			x, y, z := killed.X, killed.Y, killed.Z
			area.KillNPC(killed)
			area.SpawnDropsForNPC(killed)
			c.broadcastEmitter(area, protocol.EmitterExplosion, x, y, z, 0)
			c.broadcastSound(area, protocol.SoundNPCDeath, 200)
			return c.awardXP(ctx, int(killed.Level))
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// Cleanup on disconnect
// ---------------------------------------------------------------------------

func (c *ClientConn) cleanup(ctx context.Context) {
	if c.actor != nil {
		c.actor.Close()

		if c.state == StateInGame {
			area, ok := c.server.world.GetArea(c.actor.AreaName)
			if ok {
				area.RemoveActor(c.actor.RuntimeID)

				var w Writer
				w.WriteUint32(c.actor.RuntimeID)
				area.BroadcastAll(buildFramedPacket(protocol.PActorGone, w.Bytes()))
			}

			// Persist position.
			saveCtx := context.Background()
			if err := c.server.db.SaveCharacterPosition(saveCtx,
				c.actor.CharacterID, c.actor.AreaName,
				c.actor.X, c.actor.Y, c.actor.Z, c.actor.Yaw); err != nil {
				log.Printf("client: save position for %s: %v", c.actor.Name, err)
			}
		}
	}

	if c.account != nil {
		c.server.accounts.SetOffline(c.account.Username)
		log.Printf("client: %s disconnected", c.account.Username)
	}

	_ = c.conn.CloseWithError(0, "disconnected")
}

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------

func (c *ClientConn) sendPacket(pktType uint16, payload []byte) error {
	framed := buildFramedPacket(pktType, payload)
	c.actor.Send(framed)
	return nil
}

func (c *ClientConn) sendLoginResult(result uint8, message string) error {
	var w Writer
	w.WriteUint8(result)
	w.WriteString(message)
	return c.sendPacket(protocol.PLoginResult, w.Bytes())
}

func (c *ClientConn) sendCreateCharResult(result uint8, message string) error {
	var w Writer
	w.WriteUint8(result)
	w.WriteString(message)
	return c.sendPacket(protocol.PCreateCharResult, w.Bytes())
}

func (c *ClientConn) sendDeleteCharResult(result uint8, message string) error {
	var w Writer
	w.WriteUint8(result)
	w.WriteString(message)
	return c.sendPacket(protocol.PDeleteCharResult, w.Bytes())
}

func (c *ClientConn) sendKick(reason string) error {
	var w Writer
	w.WriteString(reason)
	return c.sendPacket(protocol.PKickedPlayer, w.Bytes())
}

// ---------------------------------------------------------------------------
// Packet building helpers
// ---------------------------------------------------------------------------

// buildFramedPacket encodes a complete framed packet into a []byte ready for
// sending. This lets us build a packet once and broadcast it to many actors.
func buildFramedPacket(pktType uint16, payload []byte) []byte {
	var w Writer
	w.WriteUint16(pktType)
	w.WriteUint32(uint32(len(payload)))
	if len(payload) > 0 {
		w.WriteRaw(payload)
	}
	// Return a copy so callers cannot alias the internal buffer.
	b := w.Bytes()
	out := make([]byte, len(b))
	copy(out, b)
	return out
}

// Note: buildNewActorPacket was removed. The single source of truth for the
// PNewActor payload is world.NewActorPayload (world/frame.go) which also
// serializes the actor's Appearance. Keeping a duplicate here caused the
// payload to drift and broke client rendering whenever fields were added.

// resultMessage returns a human-readable string for a protocol result code.
func resultMessage(result uint8) string {
	switch result {
	case protocol.ResultOK:
		return "OK"
	case protocol.ResultInvalidCreds:
		return "Invalid username or password"
	case protocol.ResultAccountExists:
		return "Username already taken"
	case protocol.ResultBanned:
		return "Account is banned"
	case protocol.ResultAlreadyOnline:
		return "Account is already online"
	case protocol.ResultServerFull:
		return "Server is full"
	case protocol.ResultCharExists:
		return "Character name already taken"
	case protocol.ResultInvalidName:
		return "Invalid name"
	default:
		return "Unknown error"
	}
}

// handleRightClick handles PRightClick — player interacts with an NPC.
func (c *ClientConn) handleRightClick(_ context.Context, payload []byte) error {
	r := NewReader(payload)
	targetRID, err := r.ReadUint32()
	if err != nil {
		return err
	}

	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if !ok {
		return nil
	}
	npc, ok := area.GetActor(targetRID)
	if !ok || !npc.IsNPC {
		return nil
	}

	// Range check — client enforces auto-walk; server validates to prevent abuse.
	const interactRange = float32(6.0)
	dx := c.actor.X - npc.X
	dz := c.actor.Z - npc.Z
	if dx*dx+dz*dz > interactRange*interactRange {
		return nil
	}

	pending := c.server.scripting.InteractNPC(c.actor, npc, area)
	if pending == nil {
		return nil
	}
	c.activeDialogNPC = targetRID
	return c.sendDialog(npc.Name, pending.Text, pending.Options)
}

// handleDialogChoice handles PDialogChoice — player picks a dialog option (0 = close).
func (c *ClientConn) handleDialogChoice(_ context.Context, payload []byte) error {
	r := NewReader(payload)
	choice, err := r.ReadUint8()
	if err != nil {
		return err
	}

	if c.activeDialogNPC == 0 {
		return nil
	}

	if choice == 0 {
		c.activeDialogNPC = 0
		return nil
	}

	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if !ok {
		c.activeDialogNPC = 0
		return nil
	}
	npc, ok := area.GetActor(c.activeDialogNPC)
	if !ok {
		c.activeDialogNPC = 0
		return nil
	}

	pending := c.server.scripting.HandleChoice(c.actor, npc, area, choice)
	if pending != nil {
		if pending.OpenShop {
			c.activeDialogNPC = 0
			return c.sendShop(npc.Name)
		}
		return c.sendDialog(npc.Name, pending.Text, pending.Options)
	}
	c.activeDialogNPC = 0
	return nil
}

// sendDialog sends a PDialog packet to this client.
func (c *ClientConn) sendDialog(npcName, text string, options []string) error {
	var w Writer
	w.WriteString(npcName)
	w.WriteString(text)
	w.WriteUint8(uint8(len(options)))
	for _, opt := range options {
		w.WriteString(opt)
	}
	return c.sendPacket(protocol.PDialog, w.Bytes())
}

// sendGoldUpdate sends the player's current gold balance.
func (c *ClientConn) sendGoldUpdate() {
	c.actor.Mu.Lock()
	gold := c.actor.Gold
	c.actor.Mu.Unlock()
	var w Writer
	w.WriteUint32(uint32(gold))
	c.actor.Send(buildFramedPacket(protocol.PGoldChange, w.Bytes()))
}

// sendWorldItems sends all dropped items currently in the area to this client.
func (c *ClientConn) sendWorldItems(area *world.Area) {
	for _, item := range area.SnapshotDroppedItems() {
		var w Writer
		w.WriteUint32(item.RuntimeID)
		w.WriteFloat32(item.X)
		w.WriteFloat32(item.Y)
		w.WriteFloat32(item.Z)
		w.WriteUint16(item.ItemID)
		w.WriteUint8(item.Quantity)
		w.WriteString(item.Name)
		w.WriteUint8(item.ItemType)
		c.actor.Send(buildFramedPacket(protocol.PWorldItem, w.Bytes()))
	}
}

// sendShop sends the shop inventory of an NPC to this client.
func (c *ClientConn) sendShop(npcName string) error {
	items := world.GetShop(npcName)
	if items == nil {
		return nil
	}
	var w Writer
	w.WriteUint8(uint8(len(items)))
	for _, it := range items {
		w.WriteUint16(it.ItemID)
		w.WriteString(it.Name)
		w.WriteUint8(it.ItemType)
		w.WriteUint8(it.SlotType)
		w.WriteUint16(uint16(it.WeaponDamage))
		w.WriteUint16(uint16(it.ArmorLevel))
		w.WriteUint32(uint32(it.BuyPrice))
		w.WriteUint32(uint32(it.SellPrice))
	}
	return c.sendPacket(protocol.POpenShop, w.Bytes())
}

// handlePickupItem handles PPickupItem — player picks up a world item.
func (c *ClientConn) handlePickupItem(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	rid, err := r.ReadUint32()
	if err != nil {
		return err
	}

	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if !ok {
		return nil
	}

	item, ok := area.GetDroppedItem(rid)
	if !ok {
		return nil
	}

	const pickupRange = float32(5.0)
	dx := c.actor.X - item.X
	dz := c.actor.Z - item.Z
	if dx*dx+dz*dz > pickupRange*pickupRange {
		return nil
	}

	if _, err := c.server.db.AddStackableItem(ctx, c.actor.CharacterID, item.ItemID, item.Quantity, 0, 100); err != nil {
		log.Printf("client: pickup AddStackableItem: %v", err)
		return nil
	}

	area.RemoveDroppedItem(rid)
	return c.sendInventory(ctx, c.actor.CharacterID)
}

// handleShopAction handles PShopAction — player buys (action=0) or sells (action=1).
func (c *ClientConn) handleShopAction(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	action, err := r.ReadUint8()
	if err != nil {
		return err
	}
	itemIDOrSlot, err := r.ReadUint16()
	if err != nil {
		return err
	}
	qty, err := r.ReadUint8()
	if err != nil {
		return err
	}
	if qty == 0 {
		qty = 1
	}

	switch action {
	case 0: // buy
		return c.handleShopBuy(ctx, itemIDOrSlot, qty)
	case 1: // sell
		return c.handleShopSell(ctx, uint8(itemIDOrSlot))
	}
	return nil
}

func (c *ClientConn) handleShopBuy(ctx context.Context, itemID uint16, qty uint8) error {
	shopItem := world.FindShopItem(itemID)
	if shopItem == nil {
		return nil
	}

	totalCost := int64(shopItem.BuyPrice) * int64(qty)

	c.actor.Mu.Lock()
	gold := c.actor.Gold
	c.actor.Mu.Unlock()

	if gold < totalCost {
		return nil // not enough gold
	}

	if _, err := c.server.db.AddStackableItem(ctx, c.actor.CharacterID, shopItem.ItemID, qty, 0, 100); err != nil {
		log.Printf("client: shop buy AddStackableItem: %v", err)
		return nil
	}

	newGold, err := c.server.db.UpdateGold(ctx, c.actor.CharacterID, -totalCost)
	if err != nil {
		log.Printf("client: shop buy UpdateGold: %v", err)
	}
	c.actor.Mu.Lock()
	c.actor.Gold = newGold
	c.actor.Mu.Unlock()

	c.sendGoldUpdate()
	return c.sendInventory(ctx, c.actor.CharacterID)
}

func (c *ClientConn) handleShopSell(ctx context.Context, slot uint8) error {
	item, err := c.server.db.GetItemAtSlot(ctx, c.actor.CharacterID, slot)
	if err != nil {
		return nil
	}

	sellPrice := int64(item.ItemValue) / 2
	if sellPrice < 1 {
		sellPrice = 1
	}

	if err := c.server.db.RemoveItemAtSlot(ctx, c.actor.CharacterID, slot); err != nil {
		log.Printf("client: shop sell RemoveItemAtSlot: %v", err)
		return nil
	}

	newGold, err := c.server.db.UpdateGold(ctx, c.actor.CharacterID, sellPrice)
	if err != nil {
		log.Printf("client: shop sell UpdateGold: %v", err)
	}
	c.actor.Mu.Lock()
	c.actor.Gold = newGold
	c.actor.Mu.Unlock()

	c.sendGoldUpdate()
	return c.sendInventory(ctx, c.actor.CharacterID)
}

// ---------------------------------------------------------------------------
// Particles / Sound / Music helpers
// ---------------------------------------------------------------------------

// broadcastEmitter sends PCreateEmitter to all players in the area.
// emitterType: protocol.EmitterXxx constant.
// durationMs: 0 = one-shot burst.
func (c *ClientConn) broadcastEmitter(area *world.Area, emitterType uint8, x, y, z float32, durationMs uint16) {
	var w Writer
	w.WriteUint8(emitterType)
	w.WriteFloat32(x)
	w.WriteFloat32(y)
	w.WriteFloat32(z)
	w.WriteUint16(durationMs)
	area.BroadcastAll(buildFramedPacket(protocol.PCreateEmitter, w.Bytes()))
}

// broadcastSound sends PSound to all players in the area.
func (c *ClientConn) broadcastSound(area *world.Area, soundID, volume uint8) {
	var w Writer
	w.WriteUint8(soundID)
	w.WriteUint8(volume)
	area.BroadcastAll(buildFramedPacket(protocol.PSound, w.Bytes()))
}

// sendSound sends PSound only to this client.
func (c *ClientConn) sendSound(soundID, volume uint8) {
	var w Writer
	w.WriteUint8(soundID)
	w.WriteUint8(volume)
	_ = c.sendPacket(protocol.PSound, w.Bytes())
}

// sendMusic sends PMusic only to this client.
func (c *ClientConn) sendMusic(trackID, volume uint8) {
	var w Writer
	w.WriteUint8(trackID)
	w.WriteUint8(volume)
	_ = c.sendPacket(protocol.PMusic, w.Bytes())
}

// areaMusicMap is populated at startup from the area_config DB table.
var areaMusicMap map[string]uint8

// SetAreaMusicMap replaces the runtime music lookup table (called once from main).
func SetAreaMusicMap(m map[string]uint8) { areaMusicMap = m }

// musicForArea returns the PMusic track ID for a given area name.
// Prefers the DB-driven map; falls back to built-in defaults.
func musicForArea(areaName string) uint8 {
	if areaMusicMap != nil {
		if track, ok := areaMusicMap[areaName]; ok {
			return track
		}
	}
	switch areaName {
	case "Forest":
		return protocol.MusicForest
	default:
		return protocol.MusicStarterZone
	}
}

// isClosedErr returns true for errors that represent a normally closed connection.
func isClosedErr(err error) bool {
	if err == nil {
		return false
	}
	var appErr *quic.ApplicationError
	if errors.As(err, &appErr) {
		return true
	}
	var idleErr *quic.IdleTimeoutError
	if errors.As(err, &idleErr) {
		return true
	}
	return false
}

