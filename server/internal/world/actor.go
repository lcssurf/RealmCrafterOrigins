package world

import (
	"log"
	"sync"
)

const sendChSize = 64

const (
	AIWait  = 0
	AIChase = 3
)

// Actor represents any entity in the game world — player or NPC.
type Actor struct {
	RuntimeID   uint32
	CharacterID string
	AccountID   string
	Username    string
	Name        string
	Race        string
	Class       string
	Level       uint16
	X, Y, Z     float32
	Yaw         float32
	AreaName    string

	// HP / EP / XP / Gold — read/write under Mu.
	Mu        sync.Mutex
	Health    int32
	HealthMax int32
	Energy    int32
	EnergyMax int32
	XP        int64
	Gold      int64

	// Combat — read/write under Mu.
	LastPortal    int64  // unix ms of last portal use (cooldown)
	AIMode        int    // AIWait or AIChase
	AITarget      *Actor // current target; nil = none
	LastAttack    int64  // unix ms of last successful attack
	LastCombatAt  int64  // unix ms of last combat action (attack/spell); 0 = never
	DeadAt        int64  // unix ms when killed; 0 = alive

	// Combat config — set once at spawn, then read-only.
	IsNPC           bool
	Aggressiveness  int     // 0=passive 1=defensive 2=aggressive 3=no-combat
	AggressiveRange float32 // detection radius; NPC starts chasing when player enters this
	AttackRange     float32 // radius at which NPC can land an attack (melee ~2, ranged ~20)
	Radius          float32
	Strength        int32
	WeaponDamage    int32
	CachedArmor     int32

	// Respawn — NPCs only, read-only after spawn.
	SpawnX, SpawnY, SpawnZ, SpawnYaw float32
	SpawnAreaName                    string
	RespawnDelay                     int64 // ms; 0 = permanent death

	// SpellCooldowns tracks last-cast timestamp per spell ID (unix ms), under Mu.
	SpellCooldowns map[uint16]int64

	// Appearance — resolved from media_actor_defs at spawn time.
	// nil = client uses its default model fallback.
	Appearance *Appearance

	// SendCh receives outbound packets for this client.
	SendCh chan []byte
	done   chan struct{}
}

// Appearance bundles the visual composition of an actor: one or more mesh
// slots (each with its own model + optional material override) and a mapping
// from high-level action names ("Idle", "Walk", …) to specific animation clip
// files. Built once at spawn time from the Media registry in the DB.
type Appearance struct {
	Meshes []MeshSlot
	Anims  []AnimBinding
}

// MeshSlot is one mesh attached to an actor. Slot values match the GUE:
// 0=Body 1=Hair 2=Helm 3=Chest 4=Hands 5=Belt 6=Legs 7=Feet 8=Weapon 9=Shield 10=Attachment.
type MeshSlot struct {
	Slot       uint8
	ModelPath  string
	Scale      float32

	// Material overrides. Empty string = use model's embedded material.
	AlbedoPath string
	NormalPath string
	ORMPath    string
	AlbedoR    float32
	AlbedoG    float32
	AlbedoB    float32
	Roughness  float32
	Metallic   float32

	// Per-aiMaterial mapping (Substance-style "blinn1"/"ID01") resolved into
	// concrete PBR paths. Used by the client to call Actor::ApplyMaterialsByName
	// after model load — every submesh that names one of these aiMaterials
	// gets the corresponding media_material's textures.
	MaterialMap []AiMaterial
}

// AiMaterial is one entry in MeshSlot.MaterialMap — the ai-material name as
// it appears in the model file plus the resolved media_material PBR paths.
type AiMaterial struct {
	AiName     string
	AlbedoPath string
	NormalPath string
	ORMPath    string
	AlbedoR    float32
	AlbedoG    float32
	AlbedoB    float32
	Roughness  float32
	Metallic   float32
}

// AnimBinding maps a game action to a concrete animation clip.
// SourcePath empty = the clip is embedded in the body model.
type AnimBinding struct {
	Action       string
	SourcePath   string
	ClipOverride string
}

// NewActor creates an Actor with initialised channels.
func NewActor() *Actor {
	return &Actor{
		SendCh: make(chan []byte, sendChSize),
		done:   make(chan struct{}),
	}
}

// ActorType returns the type byte sent in PNewActor.
// 0 = player, 1 = combat NPC, 2 = dialog-only NPC (Aggressiveness == 3).
func (a *Actor) ActorType() uint8 {
	if !a.IsNPC {
		return 0
	}
	if a.Aggressiveness == 3 {
		return 2
	}
	return 1
}

// IsDead returns true if the actor has been killed.
func (a *Actor) IsDead() bool {
	a.Mu.Lock()
	defer a.Mu.Unlock()
	return a.DeadAt > 0
}

// Send enqueues data for delivery to this actor's client.
func (a *Actor) Send(data []byte) {
	select {
	case a.SendCh <- data:
	default:
		log.Printf("world: actor %d (%s): send channel full, dropping packet", a.RuntimeID, a.Name)
	}
}

// Done returns a channel that is closed when the actor is shut down.
func (a *Actor) Done() <-chan struct{} {
	return a.done
}

// Close signals the actor's write loop to terminate.
func (a *Actor) Close() {
	select {
	case <-a.done:
	default:
		close(a.done)
	}
}
