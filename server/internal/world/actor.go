package world

import (
	"log"
	"sync"
	"sync/atomic"
	"time"
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
	// PendingImpacts: every windup/scheduled impact currently in flight for
	// this actor — replaces the old single scalar SpecialWindupUntil/
	// SpecialTargetRID/SpecialAbilityID/SpecialImpactX/Z/... fields (kept as
	// a 1-element append for the classic single-target special path, so
	// that path's behavior is unchanged; see startNPCSpecialCast,
	// combat_special.go). A slice, not a scalar, because
	// NPCCombat.spawn_impact (scripting/api.go) needs to let a boss script
	// schedule MULTIPLE concurrent impacts (a "meteor shower" pattern is
	// just N calls to spawn_impact with script-chosen positions/delays —
	// the engine has no fixed "impact_count" concept, see
	// docs/TECH_DEBT.md, mob AoE + multi-impact investigation).
	PendingImpacts        []PendingImpact
	// ActiveEffects: every currently-active status effect on this actor —
	// Fase 1 scope is CC only (stun/root/silence/slow); buffs/debuffs with
	// stat modifiers and DoT/HoT are a future phase (see
	// ActiveStatusEffect doc below). Same shape/pattern as PendingImpacts —
	// a slice of timed entries with a global ID, resolved by a periodic
	// tick (tickStatusEffects, area.go) rather than a scalar per-status
	// field, so multiple effects (e.g. slow + silence at once) can coexist
	// without one clobbering the other.
	ActiveEffects         []ActiveStatusEffect
	LastSpecialAt         int64         // unix ms of last special windup start
	LastAbilityDecisionAt int64         // unix ms of last special/script decision evaluation
	SpecialChainCount     int           // consecutive special casts in current chain window
	AbilityCooldowns      map[int]int64 // last cast start (unix ms) by ability id
	SkillLevels           map[int]int   // mastery level by ability id (player runtime cache)
	// InsideTriggers: generic-scripting Fase 1 — set of Trigger.ID this actor
	// is CURRENTLY inside, written only by Area.tickAreaTriggers (area.go)
	// under Mu. Compared against each tick's fresh containment test to
	// derive trigger_enter/trigger_exit transitions (see
	// world.SetZoneTriggerHook) — a plain "am I inside right now" scan
	// (like the older, still-present but unused CheckTrigger) can't tell
	// enter from exit on its own.
	InsideTriggers map[int]bool

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
	Derived          DerivedStats // cached computed stats from Primary + level + gear + active buffs/debuffs
	// LastItemBonuses: the itemBonuses map passed to the last full
	// RecomputeDerivedStats call (equip change/login/spawn) — cached so
	// RecomputeDerivedStatsFast (attributes.go) can cheaply re-derive
	// actor.Derived (gear + level unchanged, only ActiveEffects changed)
	// without re-querying the DB. Read/written only under Mu.
	LastItemBonuses map[string]float64
	Strength        int32        // legacy mirror of Primary.STR (temporary migration field)
	WeaponDamage    int32
	CachedArmor     int32
	BasicAttackDim  CombatDimension // dimension of the basic attack (from equipped weapon)
	// BasicAttackAnimStyle: the equipped weapon's PHYSICAL grip/pose
	// archetype (item_templates.weapon_anim_style — "sword_1h", "staff",
	// "bow"...), populated at the exact same points as BasicAttackDim
	// (login, inventory swap, use-item equip — see client.go). "" when the
	// weapon has no style configured (every weapon predates this column, and
	// most still won't have it set) — BroadcastAttack falls back to the
	// plain "Attack" action for an empty style, so this is zero-behavior-
	// change for anyone who hasn't configured a style yet.
	BasicAttackAnimStyle string

	// Respawn — NPCs only, read-only after spawn.
	SpawnX, SpawnY, SpawnZ, SpawnYaw float32
	SpawnAreaName                    string
	RespawnDelay                     int64 // ms; 0 = permanent death
	// SpawnScript (generic scripting Fase 2, NPCs only) — copied once from
	// npc_spawns.spawn_script at initial spawn (main.go), read-only after.
	// Reused as-is on every respawn (respawnNPC, area.go) — the NPC's own
	// spawn-point identity doesn't change across deaths. See
	// Registry.DispatchNPCSpawn.
	SpawnScript string

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

	// Rigid bone attachment for non-Body slots (fixed at Actor Def authoring
	// time in the GUE, not driven by equipped items). Empty BoneName = legacy
	// behaviour: the client loads/renders slot 0 only and ignores this slot
	// entirely (unchanged from before this field existed). When set, the
	// client loads this slot's model as a separate mesh and positions it via
	// Actor::GetBoneWorldTransform(BoneName) * (OffsetPos/OffsetRot/OffsetScale)
	// — the SAME two renderer primitives the item-attachment system (B5) and
	// the GUE preview's item-on-socket attachment already use, but as an
	// independent, parallel consumer: this is NOT the item/equipment socket
	// system (SocketBinding above) and does not touch it.
	BoneName    string
	OffsetPos   [3]float32
	OffsetRot   [3]float32
	OffsetScale float32
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

// PendingImpact is one scheduled windup/impact for an actor — replaces the
// old scalar Special* fields (SpecialWindupUntil/SpecialTargetRID/...). An
// actor can have any number of these in flight at once (Actor.PendingImpacts),
// which is what lets a boss script stack several concurrent/staggered
// impacts (NPCCombat.spawn_impact, scripting/api.go) without interfering
// with its normal single-target special-attack windup. See
// docs/TECH_DEBT.md, mob AoE + multi-impact investigation.
type PendingImpact struct {
	// ID: globally unique (see nextImpactID/NewImpactID below), travels to
	// the client via the windup/hit/parry/critHit combat events' meta text
	// (impact_id=N) so the client knows which of possibly SEVERAL active
	// ground telegraphs from the same source_rid to update/remove.
	ID        uint64
	ResolveAt int64 // unix ms

	// TargetRID: 0 for a "raw" scheduled impact with no personal target
	// (NPCCombat.spawn_impact) — dodge/parry/guard only run for a >0
	// TargetRID (see resolvePendingImpactEntry, combat_special.go),
	// matching the existing rule that those are personally-timed reactive
	// windows, not something a mere
	// AoE bystander gets. >0 for the classic single-target special path
	// (startNPCSpecialCast) — that target's dodge/parry/guard still apply
	// exactly as before.
	TargetRID uint32

	// ImpactX/Z: world-space impact center, captured/chosen ONCE when this
	// entry is created — never recomputed at resolution time. Single
	// source of truth for both the AoE damage radius (ActorsInRadius) and
	// the client's ground telegraph.
	ImpactX float32
	ImpactZ float32

	Ability        AbilityTemplate // real ability (classic path) or a synthetic one built from spawn_impact's args
	ActionOverride string
	ReasonTag      string
	ClientTraceID  string

	// GatesAbilityCasts: true only for entries created by the classic
	// ability-windup path (startNPCSpecialCast) — canActorStartAbilityNow
	// (cast_intent.go) checks this (not PendingImpacts' mere non-emptiness)
	// to decide "is a real special windup in progress," so a boss stacking
	// several spawn_impact() calls never blocks its own normal ability
	// rotation, and vice versa.
	GatesAbilityCasts bool
}

var nextImpactID uint64

// NewImpactID hands out a process-wide unique PendingImpact.ID. Not
// per-actor — must stay unique across the whole server so the client (which
// only ever sees a flat impact_id, not which actor "owns" the counter) can
// never collide two different actors' impacts. sync/atomic, not a mutex:
// called from arbitrary goroutines (AI tick, scripting calls) with no
// natural shared lock.
func NewImpactID() uint64 {
	return atomic.AddUint64(&nextImpactID, 1)
}

// CCType identifies which kind of crowd-control an ActiveStatusEffect with
// Kind==StatusKindCC represents. Fase 1 scope — see docs/TECH_DEBT.md,
// Buffs/Debuffs/CC investigation.
type CCType string

const (
	CCStun    CCType = "stun"    // blocks movement AND ability casts
	CCRoot    CCType = "root"    // blocks movement only
	CCSilence CCType = "silence" // blocks ability casts only
	CCSlow    CCType = "slow"    // movement speed multiplier <1, not a hard block
)

// StatusEffectKind is the broad category of an ActiveStatusEffect. Fase 1
// only ever produces StatusKindCC — StatusKindBuff/StatusKindDebuff exist
// here now so the Kind field/wire format doesn't need to change shape when
// stat-modifier buffs/debuffs and DoT/HoT are added in a future phase.
type StatusEffectKind string

const (
	StatusKindBuff   StatusEffectKind = "buff"
	StatusKindDebuff StatusEffectKind = "debuff"
	StatusKindCC     StatusEffectKind = "cc"
)

// StatMod is one flat/pct modifier a buff/debuff applies to a derived stat
// while active. Stat is a DerivedStats key — the SAME string keys already
// used by item_attributes/applyDerivedBonus (attributes.go's attributeDefs
// table), so a StatMod and a gear bonus configure identically. Flat is added
// (same as a gear bonus); Pct is a multiplier applied AFTER all flats are
// summed (0.20 = +20%), same convention as everywhere else in this codebase
// that uses a "Pct" suffix. See applyDerivedFlat/applyDerivedPct
// (attributes.go).
type StatMod struct {
	Stat string
	Flat float64
	Pct  float64
}

// ActiveStatusEffect is one currently-active status effect on an actor.
// Fase 1 populated only the CC-relevant fields (Kind, CCType, timing,
// StackRule="refresh"). Fase 2 (this round) activates StatMods for
// Kind==StatusKindBuff/StatusKindDebuff — DoT/HoT tick fields are still a
// future phase. See docs/TECH_DEBT.md, Buffs/Debuffs/CC investigation +
// Fase 1/Fase 2 implementation.
type ActiveStatusEffect struct {
	ID        uint64 // global unique id, NewImpactID() — same counter/pattern as PendingImpact.ID, travels to the client via PStatusEffectDelta
	TemplateID int    // status_effect_templates row id (GUE-authored)
	Kind      StatusEffectKind
	CCType    CCType // only meaningful when Kind==StatusKindCC
	AppliedAt int64  // unix ms
	ExpiresAt int64  // unix ms
	SourceRID uint32 // who applied it — dispel/attribution, future phase
	// StackRule: "refresh" (reapplying the same effect on the same actor
	// extends/resets ExpiresAt instead of adding a second concurrent entry),
	// "stack_up_to_max" (up to TemplateMaxStacks concurrent entries, each
	// contributing its own StatMods — see applyStackRuleLocked, actor.go),
	// or "ignore_if_active" (a new application is dropped while one is
	// already active). CC (Kind==StatusKindCC) only ever uses "refresh" —
	// stun/root/silence/slow are boolean gates, not stackable magnitudes.
	StackRule string
	// StatMods: copied from the template at apply time (not looked up live
	// at derived-stat-compute time) so a later GUE edit to the template
	// never retroactively changes an already-active effect — same
	// copy-in-at-apply-time convention as PendingImpact copying ability
	// fields. Only meaningful when Kind==StatusKindBuff/StatusKindDebuff.
	StatMods []StatMod
	// TemplateMaxStacks: copied from the template at apply time, same
	// reasoning as StatMods — needed by applyStackRuleLocked without a
	// second template lookup while holding actor.Mu.
	TemplateMaxStacks int
	// TickIntervalMs/TickDamageMin/TickDamageMax/IsHeal: Fase 3 (DoT/HoT).
	// TickIntervalMs<=0 means this effect has no tick (a plain stat buff/
	// debuff, or CC). Copied from the template at apply time, same
	// reasoning as StatMods. See tickStatusEffects (area.go).
	TickIntervalMs int64
	TickDamageMin  int32
	TickDamageMax  int32
	IsHeal         bool
	// LastTickAt: unix ms of the last tick applied (set to AppliedAt when
	// the effect is created, so the FIRST tick fires one full interval
	// AFTER application, not immediately — a 5s DoT with a 1s interval
	// ticks at +1/+2/+3/+4/+5s, 5 ticks, not 6).
	LastTickAt int64
}

// findActiveCCLocked returns the index of an existing ActiveEffects entry
// with Kind==StatusKindCC and the given CCType, or -1. Caller must hold
// actor.Mu.
func (a *Actor) findActiveCCLocked(ccType CCType) int {
	for i := range a.ActiveEffects {
		if a.ActiveEffects[i].Kind == StatusKindCC && a.ActiveEffects[i].CCType == ccType {
			return i
		}
	}
	return -1
}

// hasActiveCCLocked reports whether a still-unexpired CC of the given type
// is present. Caller must hold actor.Mu. Internal helper for the
// IsStunned/IsRooted/IsSilenced/SlowMultiplier public methods below (those
// lock Mu themselves — never call this from outside actor.go without
// already holding Mu, or you'll deadlock).
func (a *Actor) hasActiveCCLocked(ccType CCType, now int64) bool {
	i := a.findActiveCCLocked(ccType)
	return i >= 0 && a.ActiveEffects[i].ExpiresAt > now
}

// applyStackRuleLocked inserts/refreshes effect into a.ActiveEffects
// according to effect.StackRule, implementing the 3 rules designed since
// Fase 1 but only activated now (Fase 2, stat-modifier buffs/debuffs).
// Caller must hold a.Mu. Returns false if the application was dropped
// (ignore_if_active with one already active) — the caller should skip the
// broadcast/derived-stat-recompute in that case, same as a failed CC roll.
func (a *Actor) applyStackRuleLocked(effect ActiveStatusEffect) bool {
	switch effect.StackRule {
	case "ignore_if_active":
		for i := range a.ActiveEffects {
			if a.ActiveEffects[i].TemplateID == effect.TemplateID {
				return false
			}
		}
		a.ActiveEffects = append(a.ActiveEffects, effect)
		return true

	case "stack_up_to_max":
		max := effect.TemplateMaxStacks
		if max <= 0 {
			max = 1
		}
		oldestIdx, oldestExpires, count := -1, int64(0), 0
		for i := range a.ActiveEffects {
			if a.ActiveEffects[i].TemplateID != effect.TemplateID {
				continue
			}
			count++
			if oldestIdx == -1 || a.ActiveEffects[i].ExpiresAt < oldestExpires {
				oldestIdx = i
				oldestExpires = a.ActiveEffects[i].ExpiresAt
			}
		}
		if count >= max {
			// Already at the stack cap — refresh the OLDEST stack in place
			// (same magnitude, new duration) instead of growing past max.
			a.ActiveEffects[oldestIdx] = effect
		} else {
			a.ActiveEffects = append(a.ActiveEffects, effect)
		}
		return true

	default: // "refresh" — same behavior Fase 1 CC already relies on
		for i := range a.ActiveEffects {
			if a.ActiveEffects[i].TemplateID == effect.TemplateID {
				a.ActiveEffects[i] = effect
				return true
			}
		}
		a.ActiveEffects = append(a.ActiveEffects, effect)
		return true
	}
}

// ActiveStatModsLocked returns every StatMod contributed by currently
// unexpired Kind==Buff/Debuff effects (each stack of a stack_up_to_max
// effect contributes its own entry, since they're stored as separate
// ActiveEffects entries — see applyStackRuleLocked). Caller must hold a.Mu.
func (a *Actor) ActiveStatModsLocked(now int64) []StatMod {
	var mods []StatMod
	for i := range a.ActiveEffects {
		eff := &a.ActiveEffects[i]
		if eff.ExpiresAt <= now {
			continue
		}
		if eff.Kind != StatusKindBuff && eff.Kind != StatusKindDebuff {
			continue
		}
		mods = append(mods, eff.StatMods...)
	}
	return mods
}

// IsStunned reports whether a stun is currently active. Stun blocks BOTH
// movement (net/client.go's move handler) and ability casts
// (canActorStartAbilityNow, cast_intent.go).
func (a *Actor) IsStunned() bool {
	now := time.Now().UnixMilli()
	a.Mu.Lock()
	defer a.Mu.Unlock()
	return a.hasActiveCCLocked(CCStun, now)
}

// IsRooted reports whether a root (or stun, which implies root) is
// currently active. Blocks movement only — casts remain allowed under a
// pure root.
func (a *Actor) IsRooted() bool {
	now := time.Now().UnixMilli()
	a.Mu.Lock()
	defer a.Mu.Unlock()
	return a.hasActiveCCLocked(CCRoot, now) || a.hasActiveCCLocked(CCStun, now)
}

// IsSilenced reports whether a silence (or stun, which implies silence) is
// currently active. Blocks ability casts only — movement remains allowed
// under a pure silence.
func (a *Actor) IsSilenced() bool {
	now := time.Now().UnixMilli()
	a.Mu.Lock()
	defer a.Mu.Unlock()
	return a.hasActiveCCLocked(CCSilence, now) || a.hasActiveCCLocked(CCStun, now)
}

// SlowMultiplier returns the movement-speed multiplier from an active slow
// effect (1.0 = no slow, in [0,1]). Fase 1 uses one fixed multiplier
// (slowMultiplierFase1, below) rather than a per-template configurable
// strength — the status_effect_templates schema this round only has
// kind/cc_type/duration/stack_rule (see docs/TECH_DEBT.md), no "strength"
// field yet; that's a natural Fase 2 addition once slow needs to vary in
// intensity per effect.
func (a *Actor) SlowMultiplier() float32 {
	now := time.Now().UnixMilli()
	a.Mu.Lock()
	defer a.Mu.Unlock()
	if a.hasActiveCCLocked(CCSlow, now) {
		return slowMultiplierFase1
	}
	return 1.0
}

const slowMultiplierFase1 float32 = 0.5

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
	// IsTerminal: explicit "hold last frame forever, no auto-return" signal
	// (e.g. Death). Distinguishes that from ReturnTo=="" left empty BY
	// MISTAKE — the client's AnimController falls back to Idle for the
	// latter but not the former. See docs/TECH_DEBT.md, mob-death-stuck-
	// in-Idle investigation.
	IsTerminal   bool
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

// IdleAction returns the composite Idle animation action name for this
// actor's equipped weapon style ("Idle_" + BasicAttackAnimStyle) — a thin
// convenience wrapper over ComposeWeaponAction (anim_weapon_style.go), kept
// as a method since Idle is by far the most common call site (every
// BroadcastAnimate("Idle", ...) call in the codebase uses this instead).
// Falls back to plain "Idle" when no style is set — BroadcastAnimate's own
// fallback cascade (AnimFallbackParent) further walks back to "Idle" if no
// Actor Def has a specific clip bound for the composite, so this is safe
// even before any Animation Vocabulary binding exists for a given style.
// Read-only access to BasicAttackAnimStyle — call sites that already hold
// a.Mu should read the field directly instead to avoid a self-deadlock
// (this method takes no lock itself).
func (a *Actor) IdleAction() string {
	return ComposeWeaponAction(a, "Idle")
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

// TeleportToSpawn resets this actor's live position to its cached spawn
// coordinates (SpawnX/SpawnY/SpawnZ/SpawnYaw, resolved once at login — see
// handleStartGame in net/client.go, which reads the actor def's
// initial_spawn_id — and never re-derived here) and sends PRepositionActor
// so the owning client snaps immediately, mirroring exactly what
// handleRespawnPlayer already does on death (net/client.go). Returns false
// only if no valid spawn was ever resolved (SpawnX/SpawnZ still at the
// (0,0) sentinel — see the "never the 0,0,0 origin bug" fallback in
// handleStartGame), so callers (e.g. a Lua /unstuck command) can report
// failure instead of silently teleporting to the map origin.
//
// Note this returns the actor to its ORIGINAL login-area spawn, not
// necessarily a spawn point "for the area it's currently standing in" —
// SpawnAreaName may differ from AreaName if the actor has since walked
// through a portal. That matches the one existing precedent
// (handleRespawnPlayer) and avoids re-deriving spawn resolution here.
//
// hm (may be nil): the CURRENT area's heightmap, used to snap Y to the real
// terrain height at (SpawnX, SpawnZ) instead of trusting the authored
// SpawnY blindly. This matters because a player_spawns row's Y can drift
// out of sync with the terrain (re-sculpted after the spawn was placed,
// typo, copy-pasted from a different area) without anyone noticing — until
// a player actually spawns/teleports there and gets stuck: the server's
// movement-validation vertical sanity check (net/client.go
// handleStandardUpdate, mv.MaxAboveGround) rejects EVERY subsequent move
// packet whose reported Y is more than ~12 units off the real terrain
// height, so a large stored-Y/terrain mismatch here doesn't just place the
// player slightly wrong — it silently freezes their movement entirely,
// since every corrective reposition the server sends back just puts them
// right back at the same bad Y. Confirmed on the "Training Camp Spawn" row
// (player_spawns id=3): stored Y=50.32, real terrain≈34.46, a ~15.9 unit
// gap — comfortably over the 12-unit tolerance.
func (a *Actor) TeleportToSpawn(hm *Heightmap) bool {
	a.Mu.Lock()
	if a.SpawnX == 0 && a.SpawnZ == 0 {
		a.Mu.Unlock()
		return false
	}
	a.X, a.Z, a.Yaw = a.SpawnX, a.SpawnZ, a.SpawnYaw
	a.Y = a.SpawnY
	if hm != nil {
		a.Y = hm.SampleWorld(a.SpawnX, a.SpawnZ)
	}
	x, y, z, yaw := a.X, a.Y, a.Z, a.Yaw
	a.Mu.Unlock()

	var p pb
	p.u32(a.RuntimeID)
	p.f32(x)
	p.f32(y)
	p.f32(z)
	p.f32(yaw)
	a.Send(buildFrame(pRepositionActor, p))
	return true
}

// SendSystemMessage delivers a chat-log line to just this actor's client,
// tagged with sender name "System" (no other actor's name, so the client
// never mistakenly attaches a speech bubble to a real player/NPC — see
// kPChatMessage handling client-side, client/src/core/main.cpp, which
// resolves the bubble target by matching sender name against a live actor).
// Uses the same PChatMessage wire format as ordinary chat (net/client.go
// handleChatMessage): channel(u8) + sender(str) + text(str).
func (a *Actor) SendSystemMessage(text string) {
	var p pb
	p.u8(0) // channel — ignored by the client, which reads sender+text only
	p.str("System")
	p.str(text)
	a.Send(buildFrame(pChatMessage, p))
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
