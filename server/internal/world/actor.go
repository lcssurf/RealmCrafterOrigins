package world

import (
	"log"
	"sync"
)

const sendChSize = 64

const (
	AIWait        = 0
	AIPatrol      = 1
	AIWander      = 2
	AIChase       = 3
	AIPatrolPause = 4
	AIWanderPause = 5
	AIReturn      = 6
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
	Mu                sync.Mutex
	Health            int32
	HealthMax         int32
	Energy            int32 // MP
	EnergyMax         int32 // MP max
	Stamina           int32 // SP
	StaminaMax        int32 // SP max
	XP                int64
	Gold              int64
	UnspentStatPoints int32
	FreeRespecsUsed   int32

	// Combat — read/write under Mu.
	LastPortal            int64         // unix ms of last portal use (cooldown)
	LastMoveAt            int64         // unix ms of last accepted movement update
	AIMode                int           // AIWait or AIChase
	AITarget              *Actor        // current target; nil = none
	LastAttack            int64         // unix ms of last successful attack
	LastCombatAt          int64         // unix ms of last combat action (attack/spell); 0 = never
	DeadAt                int64         // unix ms when killed; 0 = alive
	Guarding              bool          // active guard stance (reduces incoming damage)
	GuardUntil            int64         // unix ms until guard stance is active
	ParryUntil            int64         // unix ms until parry window is active
	DodgeUntil            int64         // unix ms until dodge i-frames are active
	LastDodgeAt           int64         // unix ms of last dodge action
	LastGuardAt           int64         // unix ms of last guard action
	LastParryAt           int64         // unix ms of last parry action
	LastInterruptAt       int64         // unix ms of last interrupt action
	SpecialWindupUntil    int64         // unix ms when NPC special windup resolves (0 = inactive)
	SpecialTargetRID      uint32        // runtimeID of the current special target
	SpecialAbilityID      int           // active ability template id for the current special windup
	SpecialActionOverride string        // optional per-cast action override resolved from cast intent
	SpecialReasonTag      string        // trace tag for ability decisions (npc_ai/player_input/script_combo...)
	SpecialClientTraceID  string        // optional client trace correlation id
	LastSpecialAt         int64         // unix ms of last special windup start
	LastAbilityDecisionAt int64         // unix ms of last special/script decision evaluation
	SpecialChainCount     int           // consecutive special casts in current chain window
	AbilityCooldowns      map[int]int64 // last cast start (unix ms) by ability id
	SkillLevels           map[int]int   // mastery level by ability id (player runtime cache)

	// Combat config — set once at spawn, then read-only.
	SpawnID         int // source npc_spawns.id when spawned from authored spawn rows
	ActorDefID      int // visual archetype id (media_actor_defs.id)
	LootTableID     int // drop table id (media_actor_defs.loot_table_id)
	IsNPC           bool
	Aggressiveness  int     // 0=passive 1=defensive 2=aggressive 3=no-combat
	AggressiveRange float32 // detection radius; NPC starts chasing when player enters this
	AttackRange     float32 // radius at which NPC can land an attack (melee ~2, ranged ~20)
	Radius          float32
	Primary          PrimaryStats // 5 primary stats (runtime source of truth)
	EffectivePrimary PrimaryStats // base + item primary bonuses; result of last recompute. Base is in Primary. Used to display total primaries client-side.
	Derived          DerivedStats // cached computed stats from Primary + level + gear
	Strength        int32        // legacy mirror of Primary.STR (temporary migration field)
	WeaponDamage    int32
	CachedArmor     int32
	BasicAttackDim  CombatDimension // dimension of the basic attack (from equipped weapon)

	// Respawn — NPCs only, read-only after spawn.
	SpawnX, SpawnY, SpawnZ, SpawnYaw float32
	SpawnAreaName                    string
	RespawnDelay                     int64 // ms; 0 = permanent death

	// Waypoint patrol — NPCs only, set once at spawn time.
	StartWaypointID    int   // first waypoint in the route (0 = no patrol)
	CurrentWaypointID  int   // waypoint the NPC is currently heading toward
	WaypointPauseUntil int64 // unix ms when current pause ends

	// Random wander — NPCs only, set once at spawn time.
	WanderRadius     float32 // max distance from spawn for random roaming (0 = no wander)
	WanderPauseMinMs int     // minimum pause at each wander stop (ms)
	WanderPauseMaxMs int     // maximum pause at each wander stop (ms)
	WanderTargetX    float32 // current random destination X (runtime)
	WanderTargetZ    float32 // current random destination Z (runtime)

	// SpellCooldowns tracks last-cast timestamp per spell ID (unix ms), under Mu.
	SpellCooldowns map[uint16]int64

	// Appearance — resolved from media_actor_defs at spawn time.
	// nil = client uses its default model fallback.
	Appearance *Appearance

	// CurrentAction tracks the last action broadcast to clients (e.g. "Idle", "Walk", "Attack").
	// Written under Mu by BroadcastAnimate.
	CurrentAction string

	// SendCh receives outbound packets for this client.
	SendCh chan []byte
	done   chan struct{}
}

// SocketBinding maps one attachment socket name to a bone on the actor's model
// plus a local-space pos/rot/scale offset. Built from actor_def_sockets at spawn
// time. The client stores these and uses them in B5 to position attached items.
// OffsetRot is euler XYZ in degrees (converted to matrix in B5 at render time).
type SocketBinding struct {
	SocketName  string
	BoneName    string
	OffsetPos   [3]float32 // local translation (world units) relative to bone
	OffsetRot   [3]float32 // euler XYZ (degrees) relative to bone
	OffsetScale float32    // uniform scale applied to the attached item
}

// Appearance bundles the visual composition of an actor: one or more mesh
// slots (each with its own model + optional material override), a mapping
// from high-level action names ("Idle", "Walk", …) to specific animation clip
// files, and per-socket bone attachment data.
// Built once at spawn time from the Media registry in the DB.
type Appearance struct {
	Meshes    []MeshSlot
	Anims     []AnimBinding
	Sockets   []SocketBinding // B3a: per-socket bone + offset; used by client in B5
	YawOffset float32         // model-space Y rotation (degrees) applied before world yaw
	YOffset   float32         // vertical offset (world units) added to position at render time
}

// MeshSlot is one mesh attached to an actor. Slot values match the GUE:
// 0=Body 1=Hair 2=Helm 3=Chest 4=Hands 5=Belt 6=Legs 7=Feet 8=Weapon 9=Shield 10=Attachment.
type MeshSlot struct {
	Slot      uint8
	ModelPath string
	Scale     float32

	// Material overrides. Empty string = use model's embedded material.
	AlbedoPath  string
	NormalPath  string
	ORMPath     string
	AlbedoR     float32
	AlbedoG     float32
	AlbedoB     float32
	Roughness   float32
	Metallic    float32
	// BlackCutout: model-level flag (OR of model.black_cutout | material.black_cutout).
	// When true, client calls Model::ApplyBlackCutout so near-black pixels in
	// every submesh are discarded in the deferred gBuffer pass.
	BlackCutout bool

	// Per-aiMaterial mapping (Substance-style "blinn1"/"ID01") resolved into
	// concrete PBR paths. Used by the client to call Actor::OverrideMaterialsByName
	// after model load — every submesh that names one of these aiMaterials
	// gets the corresponding media_material's textures + PBR factors.
	MaterialMap []AiMaterial
}

// AiMaterial is one entry in MeshSlot.MaterialMap — the ai-material name as
// it appears in the model file plus the resolved media_material PBR paths and factors.
type AiMaterial struct {
	AiName      string
	AlbedoPath  string
	NormalPath  string
	ORMPath     string
	AlbedoR     float32
	AlbedoG     float32
	AlbedoB     float32
	Roughness   float32
	Metallic    float32
	BlackCutout bool
}

// AnimEvent is a frame-marker inside a clip that triggers a gameplay callback
// (hitbox spawn, footstep SFX, VFX, etc.) when the animation reaches that frame.
type AnimEvent struct {
	Frame     int32
	EventType string
	Payload   string
}

// AnimBinding maps a game action to a concrete animation clip with full
// playback metadata. SourcePath empty = clip is embedded in the body model.
// ClipOverride holds the FBX-native clip name when SourcePath is empty, so
// the client can alias the embedded clip to the Action name.
type AnimBinding struct {
	Action       string
	SourcePath   string
	ClipOverride string
	StartFrame   int32
	EndFrame     int32 // -1 = play to end of file
	FPS          float32
	Loop         bool
	Speed        float32
	BlendIn      float32
	ReturnTo     string
	Priority     uint8
	Events       []AnimEvent
}

// NewActor creates an Actor with initialised channels.
func NewActor() *Actor {
	return &Actor{
		SendCh:           make(chan []byte, sendChSize),
		done:             make(chan struct{}),
		AbilityCooldowns: make(map[int]int64),
		SkillLevels:      make(map[int]int),
	}
}

// SetPrimaryStats atomically updates all primary stats and keeps legacy
// Strength synchronized for compatibility during migration.
func (a *Actor) SetPrimaryStats(p PrimaryStats) {
	a.Mu.Lock()
	a.Primary = p
	a.Strength = p.STR
	a.Mu.Unlock()
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
