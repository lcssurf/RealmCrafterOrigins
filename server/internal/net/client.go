package net

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"time"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/scripting"
	"realm-crafter/server/internal/world"

	"github.com/quic-go/quic-go"
)

var defaultMovementValidation = MovementValidationConfig{
	MinDeltaSec:       0.016, // avoid extreme tiny dt
	MaxDeltaSec:       1.0,   // clamp big stalls
	BaseStepAllowance: 0.75,  // jitter/packet burst tolerance
	MaxMoveSpeed:      18.0,  // conservative sanity cap for player locomotion
	SpeedSlackMult:    1.25,  // latency slack over nominal speed budget
	MaxBelowGround:    1.0,   // allow small penetration before correction
	MaxAboveGround:    12.0,  // allow jump/fall but reject impossible heights
	EnableTelemetry:   false,
	LogRejections:     true,
	TelemetrySampleMs: 500,
}

// Connection state constants.
const (
	StateConnected     = 0
	StateAuthenticated = 1
	StateInGame        = 2

	weaponSwapCombatLockWindowMs = int64(5_000)
)

func (c *ClientConn) primaryStatsForLevel(ctx context.Context, level int) world.PrimaryStats {
	_ = c
	_ = ctx
	return world.PrimaryStatsForLevel(level)
}

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
	lastMoveLogAtMs int64
	worldEnterStart int64 // unix ms when handleStartGame started
	questLogSync    questLogSyncCache
	partySync       partySyncCache
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
		return c.dispatchInGamePacket(ctx, pktType, payload)
	}
	return nil
}

func (c *ClientConn) dispatchInGamePacket(ctx context.Context, pktType uint16, payload []byte) error {
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
	case protocol.PPlayerAction:
		return c.handlePlayerAction(ctx, payload)
	case protocol.PQuestAction:
		return c.handleQuestAction(ctx, payload)
	case protocol.PPartyAction:
		return c.handlePartyAction(ctx, payload)
	case protocol.PCombatAction:
		return c.handleCombatAction(ctx, payload)
	case protocol.PSkillLoadoutAction:
		return c.handleSkillLoadoutAction(ctx, payload)
	case protocol.PCastSkillSlot:
		return c.handleCastSkillSlot(ctx, payload)
	case protocol.PDistributeStatPoint:
		return c.handleDistributeStatPoint(ctx, payload)
	case protocol.PRespec:
		return c.handleRespec(ctx, payload)
	case protocol.PClientWorldReady:
		return c.handleClientWorldReady(payload)
	case protocol.PPing:
		return c.sendPong()
	default:
		log.Printf("client: state=ingame: unexpected packet %d", pktType)
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
	startAt := time.Now()
	c.worldEnterStart = startAt.UnixMilli()
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
	actor.Health = char.Health
	actor.HealthMax = char.HealthMax
	actor.Energy = char.Energy       // MP
	actor.EnergyMax = char.EnergyMax // MP max
	actor.StaminaMax = 100
	actor.Stamina = actor.StaminaMax
	actor.XP = char.XP
	actor.Gold = char.Gold
	actor.UnspentStatPoints = char.UnspentStatPoints
	actor.FreeRespecsUsed = char.FreeRespecsUsed
	actor.Radius = 0.4
	if wdmg, armor, err := c.server.db.GetEquippedStats(ctx, char.ID); err == nil {
		actor.WeaponDamage = wdmg
		actor.CachedArmor = armor
	}
	actor.SetPrimaryStats(world.PrimaryStats{
		STR: char.PrimaryStrength,
		DEX: char.PrimaryDexterity,
		INT: char.PrimaryIntelligence,
		WIS: char.PrimaryWisdom,
		PER: char.PrimaryPerception,
	})
	world.RecomputeDerivedStats(actor)
	actor.Mu.Lock()
	actor.Health = actor.HealthMax
	actor.Energy = actor.EnergyMax
	actor.Stamina = actor.StaminaMax
	actor.Mu.Unlock()
	actor.SpellCooldowns = make(map[uint16]int64)
	if progressRows, err := c.server.db.ListCharacterSkillProgress(ctx, char.ID); err == nil {
		actor.Mu.Lock()
		actor.SkillLevels = make(map[int]int, len(progressRows))
		for _, p := range progressRows {
			level := p.Level
			if level < 1 {
				level = 1
			}
			actor.SkillLevels[p.AbilityID] = level
		}
		actor.Mu.Unlock()
	} else {
		log.Printf("client: start game: load mastery cache char=%s: %v", char.ID, err)
	}

	// Resolve player appearance from the actor def (same path as NPCs).
	log.Printf("client: handleStartGame actor_def_id=%d for char %q", char.ActorDefID, char.Name)
	if char.ActorDefID > 0 {
		if app := BuildAppearance(ctx, c.server.db, char.ActorDefID); app != nil {
			actor.Appearance = app
			log.Printf("client: appearance resolved: %d mesh(es), %d anim(s)", len(app.Meshes), len(app.Anims))
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
	c.resetQuestLogSyncCache()
	c.resetPartySyncCache()
	c.server.registerInGameClient(c)

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
		w.WriteInt32(actor.Energy)    // MP
		w.WriteInt32(actor.EnergyMax) // MP max
		w.WriteInt32(actor.Stamina)
		w.WriteInt32(actor.StaminaMax)
		actor.Mu.Lock()
		primary := actor.Primary
		unspent := actor.UnspentStatPoints
		actor.Mu.Unlock()
		var extras Writer
		extras.WriteUint16(uint16(primary.STR))
		extras.WriteUint16(uint16(primary.DEX))
		extras.WriteUint16(uint16(primary.INT))
		extras.WriteUint16(uint16(primary.WIS))
		extras.WriteUint16(uint16(primary.PER))
		extras.WriteUint16(uint16(unspent))

		payload := append(w.Bytes(), world.AppearanceBytes(actor.Appearance)...)
		payload = append(payload, extras.Bytes()...)
		if err := c.sendPacket(protocol.PStartGame, payload); err != nil {
			return err
		}
	}

	// Send initial XP/level state.
	_ = c.sendXPUpdate()
	actor.Mu.Lock()
	primarySnapshot := actor.Primary
	unspentSnapshot := actor.UnspentStatPoints
	actor.Mu.Unlock()
	c.sendPrimaryStatsUpdate(primarySnapshot, unspentSnapshot)
	c.sendStatPointsUpdate(unspentSnapshot)

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

	// Send static world objects for this area.
	c.sendWorldObjects(area)

	// Send any dropped items already in the area.
	c.sendWorldItems(area)

	// Send gold balance.
	c.sendGoldUpdate()

	// Send known spells.
	c.sendKnownSpells()
	c.sendSkillSnapshots(ctx)

	// Send current quest state after entering world.
	_ = c.sendQuestLogSnapshot(ctx)
	_ = c.server.sendPartySnapshotToClient(c, protocol.PartyNoticeNone, "")

	// Mark the current area for explore-type objectives on login.
	c.applyQuestProgressEvent(ctx, db.QuestProgressEvent{
		ObjectiveType: db.QuestObjectiveExplore,
		TargetArea:    actor.AreaName,
		Delta:         1,
	})

	// Start area music.
	c.sendMusic(musicForArea(actor.AreaName), 128)

	// Send area environment config (skybox, etc.).
	c.sendAreaConfig(actor.AreaName)

	// Send input bindings from the default preset.
	c.sendInputBindings(ctx)

	log.Printf("perf-world-enter: user=%s char=%q area=%s server_prepare_ms=%d",
		c.account.Username, actor.Name, actor.AreaName, time.Since(startAt).Milliseconds())

	return nil
}

func (c *ClientConn) handleClientWorldReady(payload []byte) error {
	r := NewReader(payload)
	clientTotalMs, err := r.ReadUint32()
	if err != nil {
		return err
	}
	clientInitMs, err := r.ReadUint32()
	if err != nil {
		return err
	}
	var serverEndToEnd int64 = -1
	if c.worldEnterStart > 0 {
		serverEndToEnd = time.Now().UnixMilli() - c.worldEnterStart
	}
	log.Printf("perf-world-enter-ready: user=%s rid=%d client_total_ms=%d client_init_ms=%d server_e2e_ms=%d",
		c.account.Username, c.actor.RuntimeID, clientTotalMs, clientInitMs, serverEndToEnd)
	return nil
}

// sendInputBindings loads the default preset (id=1) from the DB and sends
// PInputBindings to the client.
//
// Wire format:
//
//	preset_name  str
//	count        u16
//	[count times]:
//	  context      str
//	  key          str
//	  modifier     str
//	  trigger_type str
//	  action       str
//	  axis_value   f32
//	  remappable   u8
func (c *ClientConn) sendInputBindings(ctx context.Context) {
	bindings, err := c.server.db.LoadInputBindings(ctx, 1)
	if err != nil || len(bindings) == 0 {
		return
	}

	// Filter to enabled bindings only.
	var enabled []db.InputBinding
	for _, b := range bindings {
		if b.Enabled {
			enabled = append(enabled, b)
		}
	}
	if len(enabled) == 0 {
		return
	}

	var w Writer
	w.WriteString("Default")
	w.WriteUint16(uint16(len(enabled)))
	for _, b := range enabled {
		w.WriteString(b.Context)
		w.WriteString(b.Key)
		w.WriteString(b.Modifier)
		w.WriteString(b.TriggerType)
		w.WriteString(b.Action)
		w.WriteFloat32(b.AxisValue)
		remappable := uint8(0)
		if b.Remappable {
			remappable = 1
		}
		w.WriteUint8(remappable)
	}
	_ = c.sendPacket(protocol.PInputBindings, w.Bytes())
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

	prevX, prevY, prevZ := c.actor.X, c.actor.Y, c.actor.Z
	prevYaw := c.actor.Yaw
	nowMs := time.Now().UnixMilli()
	dtSec := float64(nowMs-c.actor.LastMoveAt) / 1000.0
	if c.actor.LastMoveAt == 0 {
		dtSec = 0.1 // default client send cadence
	}
	mv := c.server.config.Movement
	if mv.MinDeltaSec <= 0 || mv.MaxDeltaSec <= 0 || mv.MaxMoveSpeed <= 0 || mv.SpeedSlackMult <= 0 {
		mv = defaultMovementValidation
	}
	if dtSec < mv.MinDeltaSec {
		dtSec = mv.MinDeltaSec
	}
	if dtSec > mv.MaxDeltaSec {
		dtSec = mv.MaxDeltaSec
	}

	var groundY float32
	hasGround := false
	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if ok && area.Heightmap != nil {
		groundY = area.Heightmap.SampleWorld(x, z)
		hasGround = true
	}

	if mv.EnableTelemetry {
		if mv.TelemetrySampleMs <= 0 {
			mv.TelemetrySampleMs = defaultMovementValidation.TelemetrySampleMs
		}
		shouldLog := nowMs-c.lastMoveLogAtMs >= mv.TelemetrySampleMs
		if shouldLog {
			dx := float64(x - prevX)
			dy := float64(y - prevY)
			dz := float64(z - prevZ)
			stepDist := math.Sqrt(dx*dx + dy*dy + dz*dz)
			if hasGround {
				yErr := float64(y - groundY)
				log.Printf("move-telemetry: player=%s rid=%d step=%.2f y_err=%.2f",
					c.actor.Name, c.actor.RuntimeID, stepDist, yErr)
			} else {
				log.Printf("move-telemetry: player=%s rid=%d step=%.2f no_heightmap",
					c.actor.Name, c.actor.RuntimeID, stepDist)
			}
			c.lastMoveLogAtMs = nowMs
		}
	}

	// Horizontal move sanity check.
	dx2D := float64(x - prevX)
	dz2D := float64(z - prevZ)
	horizDist := math.Sqrt(dx2D*dx2D + dz2D*dz2D)
	maxAllowedStep := mv.BaseStepAllowance + (mv.MaxMoveSpeed * dtSec * mv.SpeedSlackMult)

	// Vertical sanity check against terrain when this area has a heightmap.
	yInvalid := false
	var yErr float64
	if hasGround {
		yErr = float64(y - groundY)
		if yErr < -mv.MaxBelowGround || yErr > mv.MaxAboveGround {
			yInvalid = true
		}
	}

	if horizDist > maxAllowedStep || yInvalid {
		if mv.LogRejections || mv.EnableTelemetry {
			reason := "horizontal"
			if yInvalid {
				reason = "vertical"
				if horizDist > maxAllowedStep {
					reason = "horizontal+vertical"
				}
			}
			log.Printf("move-reject: player=%s rid=%d horiz=%.2f max=%.2f y_err=%.2f",
				c.actor.Name, c.actor.RuntimeID, horizDist, maxAllowedStep, yErr)
			log.Printf("move-reject-detail: player=%s rid=%d reason=%s dt=%.3f",
				c.actor.Name, c.actor.RuntimeID, reason, dtSec)
		}
		c.sendRepositionActor(c.actor.RuntimeID, prevX, prevY, prevZ, prevYaw)
		return nil
	}

	c.actor.X = x
	c.actor.Y = y
	c.actor.Z = z
	c.actor.Yaw = yaw
	c.actor.LastMoveAt = nowMs

	var w Writer
	w.WriteUint32(c.actor.RuntimeID)
	w.WriteFloat32(x)
	w.WriteFloat32(y)
	w.WriteFloat32(z)
	w.WriteFloat32(yaw)
	w.WriteUint8(flags)

	if ok {
		area.Broadcast(buildFramedPacket(protocol.PStandardUpdate, w.Bytes()), c.actor.RuntimeID)

		// Check portal triggers.
		if portal := c.server.world.CheckPortal(c.actor, area); portal != nil {
			return c.triggerPortal(area, portal)
		}
	}
	return nil
}

func (c *ClientConn) sendRepositionActor(rid uint32, x, y, z, yaw float32) {
	var rp Writer
	rp.WriteUint32(rid)
	rp.WriteFloat32(x)
	rp.WriteFloat32(y)
	rp.WriteFloat32(z)
	rp.WriteFloat32(yaw)
	c.actor.Send(buildFramedPacket(protocol.PRepositionActor, rp.Bytes()))
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

func (c *ClientConn) sendWorldObjects(area *world.Area) {
	area.Mu.RLock()
	objects := area.Objects
	area.Mu.RUnlock()
	if len(objects) == 0 {
		return
	}
	c.actor.Send(buildFramedPacket(protocol.PWorldObjects, world.WorldObjectsPayload(objects)))
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

	// Send portal positions, world objects, and dropped items in the new area.
	c.sendPortals(newArea)
	c.sendWorldObjects(newArea)
	c.sendWorldItems(newArea)

	// Resend known spells — the client clears its spellbar on PChangeArea,
	// so without this the bar stays empty after a portal.
	c.sendKnownSpells()

	// Portal FX and area music for the arriving player.
	c.sendSound(protocol.SoundPortal, 220)
	c.broadcastEmitter(newArea, protocol.EmitterPortal, portal.DestX, portal.DestY, portal.DestZ, 2000)
	c.sendMusic(musicForArea(portal.TargetArea), 128)
	c.sendAreaConfig(portal.TargetArea)

	// Mark the destination area for explore-type objectives.
	c.applyQuestProgressEvent(context.Background(), db.QuestProgressEvent{
		ObjectiveType: db.QuestObjectiveExplore,
		TargetArea:    portal.TargetArea,
		Delta:         1,
	})

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

	dmg, isCrit, onCD, result := world.ProcessAttack(c.actor, target)
	if onCD {
		return nil
	}

	died := world.BroadcastAttack(area, c.actor, target, dmg, isCrit, result)
	if died && target.IsNPC {
		x, y, z := target.X, target.Y, target.Z
		area.KillNPC(target)
		area.SpawnDropsForNPC(target)
		c.applyQuestProgressEvent(ctx, db.QuestProgressEvent{
			ObjectiveType: db.QuestObjectiveKill,
			TargetNPCName: target.Name,
			Delta:         1,
		})
		c.broadcastEmitter(area, protocol.EmitterBlood, x, y, z, 0)
		c.broadcastSound(area, protocol.SoundNPCDeath, 200)
		return c.awardXP(ctx, int(target.Level), x, z)
	}
	return nil
}

func (c *ClientConn) sendXPUpdate() error {
	c.actor.Mu.Lock()
	level := c.actor.Level
	xp := c.actor.XP
	hp := c.actor.Health
	hpMax := c.actor.HealthMax
	c.actor.Mu.Unlock()

	xpCurrentLevel := world.XPToLevel(int(level))
	xpNext := uint64(xpCurrentLevel)
	if int(level) < world.MaxCharacterLevel() {
		xpNext = uint64(world.XPToLevel(int(level) + 1))
	}
	toU32 := func(v uint64) uint32 {
		if v > math.MaxUint32 {
			return math.MaxUint32
		}
		return uint32(v)
	}

	var w Writer
	w.WriteUint8(2)
	w.WriteUint16(level)
	if xp < 0 {
		xp = 0
	}
	w.WriteUint32(toU32(uint64(xp)))
	w.WriteUint32(toU32(uint64(xpCurrentLevel)))
	w.WriteUint32(toU32(xpNext))
	if err := c.sendPacket(protocol.PXPUpdate, w.Bytes()); err != nil {
		return err
	}

	// Push HP/MP/SP so the HUD stays in sync.
	rid := c.actor.RuntimeID
	c.actor.Mu.Lock()
	mp := c.actor.Energy
	mpMax := c.actor.EnergyMax
	sp := c.actor.Stamina
	spMax := c.actor.StaminaMax
	c.actor.Mu.Unlock()

	for _, attr := range []struct {
		id  uint8
		val int32
	}{
		{1, hpMax},
		{0, hp},
		{3, mpMax},
		{2, mp},
		{5, spMax},
		{4, sp},
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

func (c *ClientConn) sendStatPointsUpdate(unspent int32) {
	if c == nil {
		return
	}
	var w Writer
	w.WriteUint16(uint16(unspent))
	_ = c.sendPacket(protocol.PStatPointsUpdate, w.Bytes())
}

func (c *ClientConn) sendPrimaryStatsUpdate(primary world.PrimaryStats, unspent int32) {
	if c == nil {
		return
	}
	var w Writer
	w.WriteUint16(uint16(primary.STR))
	w.WriteUint16(uint16(primary.DEX))
	w.WriteUint16(uint16(primary.INT))
	w.WriteUint16(uint16(primary.WIS))
	w.WriteUint16(uint16(primary.PER))
	w.WriteUint16(uint16(unspent))
	_ = c.sendPacket(protocol.PPrimaryStatsUpdate, w.Bytes())
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

func (c *ClientConn) inCombat(now int64) bool {
	if c == nil || c.actor == nil {
		return false
	}
	c.actor.Mu.Lock()
	lastCombatAt := c.actor.LastCombatAt
	c.actor.Mu.Unlock()
	return lastCombatAt > 0 && now-lastCombatAt < weaponSwapCombatLockWindowMs
}

func (c *ClientConn) clearWeaponWindup() {
	if c == nil || c.actor == nil {
		return
	}
	c.actor.Mu.Lock()
	c.actor.SpecialWindupUntil = 0
	c.actor.SpecialTargetRID = 0
	c.actor.SpecialAbilityID = 0
	c.actor.SpecialActionOverride = ""
	c.actor.SpecialReasonTag = ""
	c.actor.SpecialClientTraceID = ""
	c.actor.SpecialChainCount = 0
	c.actor.Mu.Unlock()
}

func (c *ClientConn) sendSystemChatMessage(text string) {
	if c == nil || c.actor == nil || text == "" {
		return
	}
	var w Writer
	w.WriteUint8(0)
	w.WriteString("")
	w.WriteString(text)
	_ = c.sendPacket(protocol.PChatMessage, w.Bytes())
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

	charID := c.actor.CharacterID
	weaponSlotAffected := slotA == 0 || slotB == 0
	now := time.Now().UnixMilli()
	if weaponSlotAffected && c.inCombat(now) {
		log.Printf("inventory-swap: char=%s rejected: in combat", charID)
		c.sendSystemChatMessage("Cannot change weapons in combat.")
		return c.sendInventory(ctx, charID)
	}

	swapErr := c.server.db.SwapInventorySlots(ctx, charID, slotA, slotB)
	if swapErr != nil {
		log.Printf("client: inventory swap %d↔%d: %v", slotA, slotB, swapErr)
	} else if weaponSlotAffected {
		// Weapon changed: pending windup belongs to previous weapon, cancel it.
		c.clearWeaponWindup()
	}
	// Refresh equipped combat stats after any equip change.
	if wdmg, armor, err := c.server.db.GetEquippedStats(ctx, charID); err == nil {
		c.actor.Mu.Lock()
		c.actor.WeaponDamage = wdmg
		c.actor.CachedArmor = armor
		c.actor.Mu.Unlock()
		if swapErr == nil && weaponSlotAffected {
			world.RecomputeDerivedStats(c.actor)
		}
	}
	if swapErr == nil && weaponSlotAffected {
		c.sendSkillSnapshots(ctx)
	}
	return c.sendInventory(ctx, charID)
}

func (c *ClientConn) handleUseItem(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	slot, err := r.ReadUint8()
	if err != nil {
		return err
	}

	charID := c.actor.CharacterID
	wouldEquipWeapon := false
	if slot >= 14 {
		items, invErr := c.server.db.GetInventory(ctx, charID)
		if invErr == nil {
			for _, ci := range items {
				if ci != nil && ci.Slot == slot && ci.SlotType == 0 && ci.ItemType != 2 {
					wouldEquipWeapon = true
					break
				}
			}
		}
	}
	now := time.Now().UnixMilli()
	if wouldEquipWeapon && c.inCombat(now) {
		log.Printf("use-item: char=%s rejected: weapon change in combat", charID)
		c.sendSystemChatMessage("Cannot change weapons in combat.")
		return c.sendInventory(ctx, charID)
	}

	res, err := c.server.db.UseItem(ctx, charID, slot)
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
		if wdmg, armor, err := c.server.db.GetEquippedStats(ctx, charID); err == nil {
			c.actor.Mu.Lock()
			c.actor.WeaponDamage = wdmg
			c.actor.CachedArmor = armor
			c.actor.Mu.Unlock()
			world.RecomputeDerivedStats(c.actor)
		}
		if res.EquipSlot == 0 {
			// Weapon changed: pending windup belongs to previous weapon, cancel it.
			c.clearWeaponWindup()
			c.sendSkillSnapshots(ctx)
		}
	}

	return c.sendInventory(ctx, charID)
}

func (c *ClientConn) handleRespawnPlayer(ctx context.Context) error {
	c.actor.Mu.Lock()
	if c.actor.Health > 0 {
		c.actor.Mu.Unlock()
		return nil // not dead — ignore
	}
	c.actor.Health = c.actor.HealthMax
	c.actor.DeadAt = 0
	c.actor.Guarding = false
	c.actor.GuardUntil = 0
	c.actor.ParryUntil = 0
	c.actor.DodgeUntil = 0
	c.actor.SpecialWindupUntil = 0
	c.actor.SpecialTargetRID = 0
	c.actor.SpecialAbilityID = 0
	c.actor.SpecialActionOverride = ""
	c.actor.SpecialReasonTag = ""
	c.actor.SpecialClientTraceID = ""
	c.actor.SpecialChainCount = 0
	c.actor.AbilityCooldowns = make(map[int]int64)
	c.actor.LastCombatAt = 0
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
		world.BroadcastAnimate(area, c.actor, "Idle")
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

// sendSkillSnapshots sends both active hotbar state and active kit pool snapshots.
func (c *ClientConn) sendSkillSnapshots(ctx context.Context) {
	c.sendSkillState(ctx)
	c.sendKitPool(ctx)
}

// sendSkillState resolves the active player kit snapshot and sends PSkillState.
func (c *ClientConn) sendSkillState(ctx context.Context) {
	if c == nil || c.server == nil || c.server.db == nil || c.actor == nil {
		return
	}
	charID := c.actor.CharacterID
	if charID == "" {
		return
	}

	resolution, err := c.server.db.ResolveActivePlayerKit(ctx, charID)
	if err != nil {
		log.Printf("sendSkillState: resolve failed for char %s: %v", charID, err)
		return
	}

	progressByAbilityID := make(map[int]*db.CharacterSkillProgress)
	progressRows, err := c.server.db.ListCharacterSkillProgress(ctx, charID)
	if err != nil {
		log.Printf("sendSkillState: load progress failed for char %s: %v", charID, err)
	} else {
		for _, row := range progressRows {
			if row == nil || row.AbilityID <= 0 {
				continue
			}
			progressByAbilityID[row.AbilityID] = row
		}
	}

	cachedLevels := make(map[int]int)
	cooldownStartedAtByAbilityID := make(map[int]int64)
	c.actor.Mu.Lock()
	for abilityID, level := range c.actor.SkillLevels {
		cachedLevels[abilityID] = level
	}
	for abilityID, lastCastAt := range c.actor.AbilityCooldowns {
		cooldownStartedAtByAbilityID[abilityID] = lastCastAt
	}
	c.actor.Mu.Unlock()
	nowMs := time.Now().UnixMilli()

	payload := PSkillStatePayload{
		Version:        4,
		HasKit:         resolution.HasKit,
		KitID:          0,
		KitKey:         resolution.KitKey,
		KitDisplayName: resolution.KitDisplayName,
		Abilities:      make([]PSkillStateAbility, 0, len(resolution.Abilities)),
	}
	if resolution.KitID > 0 {
		payload.KitID = uint32(resolution.KitID)
	}

	for _, ab := range resolution.Abilities {
		if ab.SlotIndex < 0 || ab.SlotIndex > 255 {
			log.Printf("sendSkillState: skip invalid slot_index=%d for char=%s ability_id=%d", ab.SlotIndex, charID, ab.AbilityID)
			continue
		}
		if ab.AbilityID < 0 {
			log.Printf("sendSkillState: skip invalid ability_id=%d for char=%s", ab.AbilityID, charID)
			continue
		}
		cdMs := ab.CooldownMs
		if cdMs < 0 {
			cdMs = 0
		}
		if cdMs > math.MaxUint32 {
			cdMs = math.MaxUint32
		}

		maxLevel := 10
		var abilityTemplate *world.AbilityTemplate
		abilityDescription := ""
		if tpl, ok := world.GetAbilityTemplateByID(ab.AbilityID); ok {
			abilityTemplate = &tpl
			abilityDescription = tpl.Description
			if tpl.MasteryMaxLevel > 0 {
				maxLevel = tpl.MasteryMaxLevel
			}
		}
		if maxLevel < 1 {
			maxLevel = 1
		}
		if maxLevel > math.MaxUint8 {
			maxLevel = math.MaxUint8
		}

		level := 1
		if cachedLevel, ok := cachedLevels[ab.AbilityID]; ok && cachedLevel > 0 {
			level = cachedLevel
		} else if row := progressByAbilityID[ab.AbilityID]; row != nil && row.Level > 0 {
			level = row.Level
		}
		if level < 1 {
			level = 1
		}
		if level > maxLevel {
			level = maxLevel
		}
		if level > math.MaxUint8 {
			level = math.MaxUint8
		}

		var masteryXP uint32
		if row := progressByAbilityID[ab.AbilityID]; row != nil && row.XP > 0 {
			if row.XP > math.MaxUint32 {
				masteryXP = math.MaxUint32
			} else {
				masteryXP = uint32(row.XP)
			}
		}

		var masteryXPForNext uint32
		var masteryXPCurrentLevelThreshold uint32
		if abilityTemplate != nil && level < maxLevel {
			nextXPRequired := db.XPRequiredForLevelFromAbility(level+1, abilityTemplate)
			if nextXPRequired > 0 {
				if nextXPRequired > math.MaxUint32 {
					masteryXPForNext = math.MaxUint32
				} else {
					masteryXPForNext = uint32(nextXPRequired)
				}
			}
		}
		if abilityTemplate != nil {
			curXPRequired := db.XPRequiredForLevelFromAbility(level, abilityTemplate)
			if curXPRequired > math.MaxUint32 {
				masteryXPCurrentLevelThreshold = math.MaxUint32
			} else if curXPRequired > 0 {
				masteryXPCurrentLevelThreshold = uint32(curXPRequired)
			}
		}

		cooldownRemainingMs := uint32(0)
		if lastCastAt, ok := cooldownStartedAtByAbilityID[ab.AbilityID]; ok && lastCastAt > 0 {
			effectiveCooldownMs := int64(cdMs)
			if abilityTemplate != nil {
				effectiveCooldownMs = world.EffectiveCooldownMs(c.actor, *abilityTemplate)
			}
			if effectiveCooldownMs > 0 {
				elapsed := nowMs - lastCastAt
				if elapsed < 0 {
					elapsed = 0
				}
				if elapsed < effectiveCooldownMs {
					remaining := effectiveCooldownMs - elapsed
					if remaining > math.MaxUint32 {
						cooldownRemainingMs = math.MaxUint32
					} else {
						cooldownRemainingMs = uint32(remaining)
					}
				}
			}
		}

		payload.Abilities = append(payload.Abilities, PSkillStateAbility{
			SlotIndex:                      uint8(ab.SlotIndex),
			AbilityID:                      uint32(ab.AbilityID),
			AbilityName:                    ab.AbilityName,
			CooldownMs:                     uint32(cdMs),
			CooldownRemainingMs:            cooldownRemainingMs,
			MasteryLevel:                   uint8(level),
			MasteryXP:                      masteryXP,
			MasteryXPCurrentLevelThreshold: masteryXPCurrentLevelThreshold,
			MasteryXPForNext:               masteryXPForNext,
			MasteryMaxLevel:                uint8(maxLevel),
			Description:                    abilityDescription,
		})
	}

	buf, err := EncodePSkillState(payload)
	if err != nil {
		log.Printf("sendSkillState: encode failed for char %s: %v", charID, err)
		return
	}
	if err := c.sendPacket(protocol.PSkillState, buf); err != nil {
		log.Printf("sendSkillState: send failed for char %s: %v", charID, err)
		return
	}
	log.Printf("sendSkillState: sent for char=%s has_kit=%t kit_id=%d kit=%s abilities=%d",
		charID, payload.HasKit, payload.KitID, payload.KitKey, len(payload.Abilities))
}

// sendKitPool resolves the active kit identity and sends its full enabled ability pool.
func (c *ClientConn) sendKitPool(ctx context.Context) {
	if c == nil || c.server == nil || c.server.db == nil || c.actor == nil {
		return
	}
	charID := c.actor.CharacterID
	if charID == "" {
		return
	}

	resolution, err := c.server.db.ResolveActivePlayerKit(ctx, charID)
	if err != nil {
		log.Printf("sendKitPool: resolve failed for char %s: %v", charID, err)
		return
	}

	payload := PKitPoolPayload{
		Version:        1,
		KitID:          0,
		KitKey:         "",
		KitDisplayName: "",
		Abilities:      nil,
	}
	if resolution.HasKit && resolution.KitID > 0 {
		poolEntries, err := c.server.db.ListEnabledKitPoolAbilities(ctx, resolution.KitID)
		if err != nil {
			log.Printf("sendKitPool: list pool failed for char=%s kit_id=%d: %v", charID, resolution.KitID, err)
			return
		}

		payload.KitID = uint32(resolution.KitID)
		payload.KitKey = resolution.KitKey
		payload.KitDisplayName = resolution.KitDisplayName
		payload.Abilities = make([]PKitPoolAbility, 0, len(poolEntries))
		for _, ab := range poolEntries {
			if ab.AbilityID < 0 {
				log.Printf("sendKitPool: skip invalid ability_id=%d for char=%s kit_id=%d", ab.AbilityID, charID, resolution.KitID)
				continue
			}
			cdMs := ab.CooldownMs
			if cdMs < 0 {
				cdMs = 0
			}
			if cdMs > math.MaxUint32 {
				cdMs = math.MaxUint32
			}
			payload.Abilities = append(payload.Abilities, PKitPoolAbility{
				AbilityID:   uint32(ab.AbilityID),
				AbilityName: ab.AbilityName,
				CooldownMs:  uint32(cdMs),
			})
		}
	}

	buf, err := EncodePKitPool(payload)
	if err != nil {
		log.Printf("sendKitPool: encode failed for char %s: %v", charID, err)
		return
	}
	if err := c.sendPacket(protocol.PKitPool, buf); err != nil {
		log.Printf("sendKitPool: send failed for char %s: %v", charID, err)
		return
	}
	log.Printf("sendKitPool: sent for char=%s kit_id=%d kit=%s abilities=%d",
		charID, payload.KitID, payload.KitKey, len(payload.Abilities))
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

	area, ok := c.server.world.GetArea(c.actor.AreaName)
	if !ok {
		return nil
	}
	var target *world.Actor
	if targetRID != 0 {
		target, _ = area.GetActor(targetRID)
	}

	// Optional C2 bridge: when runtime_ability_id is configured for this spell,
	// cast through the unified cast_intent pipeline instead of legacy spell Lua.
	if def.RuntimeAbilityID > 0 {
		reasonTag := "player_input"
		actionOverride := ""
		clientTraceID := ""
		if c.server.scripting != nil {
			advice := c.server.scripting.DispatchPlayerBeforeCastIntent(
				area,
				c.actor,
				target,
				def.RuntimeAbilityID,
				reasonTag,
			)
			if advice.Cancel {
				log.Printf("client: %s cast-intent cancelled by script spell=%d runtime_ability=%d target=%d",
					c.actor.Name, def.ID, def.RuntimeAbilityID, targetRID)
				return nil
			}
			if advice.ReasonTag != "" {
				reasonTag = advice.ReasonTag
			}
			actionOverride = advice.ActionOverride
			clientTraceID = advice.ClientTraceID
		}

		started, reason := world.TryStartPlayerCastByRID(c.server.world, world.CastIntent{
			CasterRID:      c.actor.RuntimeID,
			TargetRID:      targetRID,
			AbilityID:      def.RuntimeAbilityID,
			ActionOverride: actionOverride,
			ReasonTag:      reasonTag,
			ClientTraceID:  clientTraceID,
		})
		if !started {
			log.Printf("client: %s cast-intent reject spell=%d runtime_ability=%d target=%d reason=%s",
				c.actor.Name, def.ID, def.RuntimeAbilityID, targetRID, reason)
			return nil
		}
		// Provoke defensive/aggressive NPC target on successful cast start.
		if target != nil && target.IsNPC {
			target.Mu.Lock()
			if target.AIMode == world.AIWait &&
				(target.Aggressiveness == 1 || target.Aggressiveness == 2) {
				target.AIMode = world.AIChase
				target.AITarget = c.actor
			}
			target.Mu.Unlock()
		}
		log.Printf("client: %s casting %q via cast-intent (spell=%d runtime_ability=%d target=%d)",
			c.actor.Name, def.Name, def.ID, def.RuntimeAbilityID, targetRID)
		return nil
	}

	// Validate and deduct MP + cooldown under lock.
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
	mp := c.actor.Energy
	c.actor.Mu.Unlock()

	// Broadcast MP update immediately.
	world.BroadcastMPUpdate(c.actor, mp)

	needsTarget := def.SpellType == "damage" || def.SpellType == "debuff"
	isGroundAoE := def.AoEType == 2

	if needsTarget && !isGroundAoE {
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
			c.applyQuestProgressEvent(ctx, db.QuestProgressEvent{
				ObjectiveType: db.QuestObjectiveKill,
				TargetNPCName: killed.Name,
				Delta:         1,
			})
			c.broadcastEmitter(area, protocol.EmitterBlood, x, y, z, 0)
			c.broadcastSound(area, protocol.SoundNPCDeath, 200)
			return c.awardXP(ctx, int(killed.Level), x, z)
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
			c.handlePartyDisconnect()
			c.server.unregisterInGameClient(c)

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
		} else {
			c.server.unregisterInGameClient(c)
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

	// NPC interaction can advance "talk to NPC" objectives.
	c.applyQuestProgressEvent(context.Background(), db.QuestProgressEvent{
		ObjectiveType: db.QuestObjectiveTalk,
		TargetNPCName: npc.Name,
		Delta:         1,
	})
	// Interact objectives share the same NPC target matching contract.
	c.applyQuestProgressEvent(context.Background(), db.QuestProgressEvent{
		ObjectiveType: db.QuestObjectiveInteract,
		TargetNPCName: npc.Name,
		Delta:         1,
	})

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

	c.applyQuestProgressEvent(ctx, db.QuestProgressEvent{
		ObjectiveType: db.QuestObjectiveCollect,
		TargetItemID:  item.ItemID,
		Delta:         int(item.Quantity),
	})

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

// areaConfigMap holds the full AreaConfig per area name (populated once from main).
var areaConfigMap map[string]*db.AreaConfig

// SetAreaConfigMap replaces the runtime area-config lookup (called once from main).
func SetAreaConfigMap(m map[string]*db.AreaConfig) { areaConfigMap = m }

// sendAreaConfig sends PAreaConfig to this client with the skybox HDR for the area.
func (c *ClientConn) sendAreaConfig(areaName string) {
	cfg := db.AreaConfig{
		SkyboxHdr:               "",
		SunDirX:                 0.18,
		SunDirY:                 0.96,
		SunDirZ:                 0.20,
		SunColorR:               1.14,
		SunColorG:               1.12,
		SunColorB:               1.05,
		SunIntensityMul:         1.00,
		SkyIntensityMul:         1.00,
		FogDensityMul:           0.92,
		FogR:                    0.70,
		FogG:                    0.80,
		FogB:                    0.93,
		Volumetrics:             true,
		CharShadowLift:          0.30,
		CharRimStrength:         0.18,
		CharRimExponent:         2.40,
		CharMinNdotL:            0.10,
		CharAmbientBoost:        0.12,
		SceneIblIntensity:       1.00,
		SceneSkyIntensity:       1.16,
		SceneWorldShadowLift:    0.10,
		SceneDirectScale:        1.32,
		SceneAmbientScale:       0.88,
		SceneFlatAmbient:        0.03,
		SceneWorldMinNdotL:      0.05,
		SceneAlbedoMinLuma:      0.18,
		SceneAlbedoLiftStrength: 0.00,
		SceneSpecularScale:      0.88,
		SceneExposureFactor:     1.10,
		SceneSunIntensity:       1.36,
		ColorContrast:           1.08,
		ColorSaturation:         1.08,
		ColorVibrance:           0.20,
		ColorBlackPoint:         0.010,
		ColorVignetteStrength:   0.04,
		ColorVignetteSoftness:   0.55,
		TerrainTilingMul:        1.00,
		TerrainMacroStrengthMul: 1.00,
		TerrainHeightBlendSlop:  0.20,
	}
	if areaConfigMap != nil {
		if areaCfg, ok := areaConfigMap[areaName]; ok && areaCfg != nil {
			cfg = *areaCfg
		}
	}

	clampf := func(v, mn, mx float32) float32 {
		if v < mn {
			return mn
		}
		if v > mx {
			return mx
		}
		return v
	}
	sunDirX, sunDirY, sunDirZ := cfg.SunDirX, cfg.SunDirY, cfg.SunDirZ
	sunLen := float32(math.Sqrt(float64(sunDirX*sunDirX + sunDirY*sunDirY + sunDirZ*sunDirZ)))
	if sunLen < 0.0001 {
		sunDirX, sunDirY, sunDirZ = 0.18, 0.96, 0.20
		sunLen = float32(math.Sqrt(float64(sunDirX*sunDirX + sunDirY*sunDirY + sunDirZ*sunDirZ)))
	}
	inv := float32(1.0) / sunLen
	sunDirX *= inv
	sunDirY *= inv
	sunDirZ *= inv

	var w Writer
	w.WriteString(cfg.SkyboxHdr)
	w.WriteFloat32(sunDirX)
	w.WriteFloat32(sunDirY)
	w.WriteFloat32(sunDirZ)
	w.WriteFloat32(clampf(cfg.SunColorR, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SunColorG, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SunColorB, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SunIntensityMul, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SkyIntensityMul, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.FogDensityMul, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.FogR, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.FogG, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.FogB, 0.0, 2.0))
	w.WriteBool(cfg.Volumetrics)
	w.WriteFloat32(clampf(cfg.CharShadowLift, 0.0, 1.0))
	w.WriteFloat32(clampf(cfg.CharRimStrength, 0.0, 1.0))
	w.WriteFloat32(clampf(cfg.CharRimExponent, 1.0, 6.0))
	w.WriteFloat32(clampf(cfg.CharMinNdotL, 0.0, 0.5))
	w.WriteFloat32(clampf(cfg.CharAmbientBoost, 0.0, 0.5))
	w.WriteFloat32(clampf(cfg.SceneIblIntensity, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SceneSkyIntensity, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SceneWorldShadowLift, 0.0, 0.95))
	w.WriteFloat32(clampf(cfg.SceneDirectScale, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SceneAmbientScale, 0.0, 3.0))
	w.WriteFloat32(clampf(cfg.SceneFlatAmbient, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SceneWorldMinNdotL, 0.0, 1.0))
	w.WriteFloat32(clampf(cfg.SceneAlbedoMinLuma, 0.0, 1.0))
	w.WriteFloat32(clampf(cfg.SceneAlbedoLiftStrength, 0.0, 1.0))
	w.WriteFloat32(clampf(cfg.SceneSpecularScale, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.SceneExposureFactor, 0.05, 2.0))
	w.WriteFloat32(clampf(cfg.SceneSunIntensity, 0.0, 2.0))
	w.WriteFloat32(clampf(cfg.ColorContrast, 0.80, 1.35))
	w.WriteFloat32(clampf(cfg.ColorSaturation, 0.80, 1.40))
	w.WriteFloat32(clampf(cfg.ColorVibrance, -0.30, 0.60))
	w.WriteFloat32(clampf(cfg.ColorBlackPoint, 0.0, 0.06))
	w.WriteFloat32(clampf(cfg.ColorVignetteStrength, 0.0, 0.20))
	w.WriteFloat32(clampf(cfg.ColorVignetteSoftness, 0.0, 1.0))
	w.WriteFloat32(clampf(cfg.TerrainTilingMul, 0.50, 2.50))
	w.WriteFloat32(clampf(cfg.TerrainMacroStrengthMul, 0.00, 3.00))
	w.WriteFloat32(clampf(cfg.TerrainHeightBlendSlop, 0.02, 0.70))
	_ = c.sendPacket(protocol.PAreaConfig, w.Bytes())
}

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

// handlePlayerAction processes PPlayerAction (C→S). The client sends an action
// string, a state byte (0=press, 1=hold_start, 2=hold_end), and an axis value.
// For press/hold_start, the action is validated and propagated as PAnimateActor
// to all clients in the area. Future: stamina, stun, grounded checks.
func (c *ClientConn) handlePlayerAction(ctx context.Context, payload []byte) error {
	r := NewReader(payload)
	action, err := r.ReadString()
	if err != nil {
		return nil // malformed — ignore
	}
	state, err := r.ReadUint8()
	if err != nil {
		return nil
	}
	_, err = r.ReadFloat32() // axis_value — reserved for future use
	if err != nil {
		return nil
	}

	if c.actor == nil || c.actor.IsDead() {
		return nil
	}

	area := c.server.world.GetOrCreateArea(c.actor.AreaName)
	if area == nil {
		return nil
	}

	// Propagate press/hold_start as an animation action to all clients.
	if state == 0 || state == 1 {
		world.BroadcastAnimate(area, c.actor, action)
		// Fire scripting event for server-side validation/effects.
		c.server.scripting.DispatchPlayerAction(c.actor, action, state)
	}
	return nil
}

// sendInputContext sends PSetInputContext to this client only.
func (c *ClientConn) sendInputContext(ctxName string) {
	var w Writer
	w.WriteString(ctxName)
	_ = c.sendPacket(protocol.PSetInputContext, w.Bytes())
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
