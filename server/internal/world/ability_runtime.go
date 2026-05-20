package world

import (
	"sort"
	"sync"
)

// AbilityTemplate is one authoritative combat ability definition loaded from DB.
// NPC and player intents resolve against this catalog.
type AbilityTemplate struct {
	ID                         int
	Name                       string
	Description                string
	Family                     string
	Category                   string
	ResourceType               string
	ResourceCost               int32
	CooldownMs                 int64
	RangeMin                   float32
	RangeMax                   float32
	WindupMs                   int64
	ImpactDelayMs              int64
	RecoverMs                  int64
	ParryWindowMs              int64
	Interruptible              bool
	BaseDamageMin              int32
	BaseDamageMax              int32
	DamageStatScaleJSON        string
	ArmorPiercePct             float32
	CritPolicyJSON             string
	TelegraphType              string
	TelegraphRadius            float32
	TelegraphColorRGBA         string
	ActionWindup               string
	ActionImpact               string
	ActionRecover              string
	AllowActionOverride        bool
	AllowedActionTagsJSON      string
	VFXIDWindup                int
	VFXIDImpact                int
	SFXIDWindup                int
	SFXIDImpact                int
	MasteryXPPerUse            int
	MasteryMaxLevel            int
	MasteryXPCurveType         string
	MasteryXPCurveBase         int
	MasteryXPCurveExponent     float64
	MasteryXPIrregularity      float64
	MasteryPrimaryBonusPerLvl  float64
	MasteryCooldownReduxPerLvl float64
	Enabled                    bool
}

// NPCAbilityLoadoutEntry maps NPCs/archetypes to ability templates and simple
// AI selection constraints.
type NPCAbilityLoadoutEntry struct {
	ID             int
	NPCSpawnID     int
	ActorDefID     int
	AbilityID      int
	Priority       int
	Weight         int
	MinDistance    float32
	MaxDistance    float32
	MinTargetHPPct float32
	MaxTargetHPPct float32
	PhaseTag       string
	ConditionLua   string
	Enabled        bool
}

// NPCCombatProfile defines shared behavior cadence and constraints for NPC
// ability decision loops.
type NPCCombatProfile struct {
	ID                     int
	Name                   string
	GlobalGCDMs            int64
	DecisionTickMs         int64
	AggroStyle             string
	AllowChainCast         bool
	MaxConsecutiveSpecials int
	Enabled                bool
}

// NPCProfileBinding selects which profile applies to a specific spawn/archetype.
type NPCProfileBinding struct {
	ID         int
	NPCSpawnID int
	ActorDefID int
	ProfileID  int
	Enabled    bool
}

// NPCAbilityDecisionContext is a read-only snapshot used by scripts for
// condition checks and ability selection telemetry.
type NPCAbilityDecisionContext struct {
	Distance    float32
	NPCHPPct    float32
	TargetHPPct float32
	NPCSPPct    float32
	TargetSPPct float32
	NPCMPPct    float32
	TargetMPPct float32
	PhaseTag    string
	Phase       int
}

var defaultNPCCombatProfile = NPCCombatProfile{
	ID:                     0,
	Name:                   "default_profile",
	GlobalGCDMs:            450,
	DecisionTickMs:         250,
	AggroStyle:             "default",
	AllowChainCast:         false,
	MaxConsecutiveSpecials: 1,
	Enabled:                true,
}

var (
	abilityRuntimeMu         sync.RWMutex
	abilityRuntimeEnabled    = true
	abilityTemplatesByID     = map[int]AbilityTemplate{}
	abilityLoadoutsBySpawn   = map[int][]NPCAbilityLoadoutEntry{}
	abilityLoadoutsByActorID = map[int][]NPCAbilityLoadoutEntry{}
	profilesByID             = map[int]NPCCombatProfile{}
	profileBindingBySpawn    = map[int]NPCProfileBinding{}
	profileBindingByActorID  = map[int]NPCProfileBinding{}
	defaultProfileID         = 0
	npcDecisionHook          func(area *Area, npc *Actor, target *Actor, now int64) bool
)

// SetAbilityRuntimeEnabled controls whether NPC scripted ability selection is
// used by the world combat loop. When disabled, legacy fallback behavior is
// used to preserve compatibility.
func SetAbilityRuntimeEnabled(enabled bool) {
	abilityRuntimeMu.Lock()
	abilityRuntimeEnabled = enabled
	abilityRuntimeMu.Unlock()
}

func abilityRuntimeIsEnabled() bool {
	abilityRuntimeMu.RLock()
	defer abilityRuntimeMu.RUnlock()
	return abilityRuntimeEnabled
}

// SetAbilityCatalog replaces the in-memory ability template catalog.
func SetAbilityCatalog(templates []AbilityTemplate) {
	next := make(map[int]AbilityTemplate, len(templates))
	for _, t := range templates {
		if t.ID <= 0 {
			continue
		}
		next[t.ID] = t
	}
	abilityRuntimeMu.Lock()
	abilityTemplatesByID = next
	abilityRuntimeMu.Unlock()
}

// SetNPCAbilityLoadouts replaces loadout mappings for NPC spawn IDs and actor
// definition IDs. Ordering is normalized by priority, then weight.
func SetNPCAbilityLoadouts(loadouts []NPCAbilityLoadoutEntry) {
	bySpawn := make(map[int][]NPCAbilityLoadoutEntry)
	byActor := make(map[int][]NPCAbilityLoadoutEntry)
	for _, l := range loadouts {
		if l.AbilityID <= 0 || !l.Enabled {
			continue
		}
		if l.NPCSpawnID > 0 {
			bySpawn[l.NPCSpawnID] = append(bySpawn[l.NPCSpawnID], l)
		}
		if l.ActorDefID > 0 {
			byActor[l.ActorDefID] = append(byActor[l.ActorDefID], l)
		}
	}
	sortFn := func(items []NPCAbilityLoadoutEntry) {
		sort.SliceStable(items, func(i, j int) bool {
			if items[i].Priority != items[j].Priority {
				return items[i].Priority > items[j].Priority
			}
			if items[i].Weight != items[j].Weight {
				return items[i].Weight > items[j].Weight
			}
			return items[i].AbilityID < items[j].AbilityID
		})
	}
	for k := range bySpawn {
		sortFn(bySpawn[k])
	}
	for k := range byActor {
		sortFn(byActor[k])
	}
	abilityRuntimeMu.Lock()
	abilityLoadoutsBySpawn = bySpawn
	abilityLoadoutsByActorID = byActor
	abilityRuntimeMu.Unlock()
}

func resolveAbilityTemplate(abilityID int) (AbilityTemplate, bool) {
	abilityRuntimeMu.RLock()
	defer abilityRuntimeMu.RUnlock()
	t, ok := abilityTemplatesByID[abilityID]
	return t, ok
}

// GetAbilityTemplateByID returns one ability template from the in-memory runtime catalog.
func GetAbilityTemplateByID(abilityID int) (AbilityTemplate, bool) {
	return resolveAbilityTemplate(abilityID)
}

func resolveNPCAbilityLoadout(spawnID, actorDefID int) []NPCAbilityLoadoutEntry {
	abilityRuntimeMu.RLock()
	defer abilityRuntimeMu.RUnlock()
	if spawnID > 0 {
		if rows := abilityLoadoutsBySpawn[spawnID]; len(rows) > 0 {
			out := make([]NPCAbilityLoadoutEntry, len(rows))
			copy(out, rows)
			return out
		}
	}
	if actorDefID > 0 {
		if rows := abilityLoadoutsByActorID[actorDefID]; len(rows) > 0 {
			out := make([]NPCAbilityLoadoutEntry, len(rows))
			copy(out, rows)
			return out
		}
	}
	return nil
}

// SetNPCCombatProfiles replaces the in-memory combat profile catalog.
func SetNPCCombatProfiles(profiles []NPCCombatProfile) {
	next := make(map[int]NPCCombatProfile, len(profiles))
	defaultID := 0
	for _, p := range profiles {
		if p.ID <= 0 || !p.Enabled {
			continue
		}
		if p.GlobalGCDMs < 0 {
			p.GlobalGCDMs = 0
		}
		if p.DecisionTickMs < 0 {
			p.DecisionTickMs = 0
		}
		if p.MaxConsecutiveSpecials < 1 {
			p.MaxConsecutiveSpecials = 1
		}
		next[p.ID] = p
		if p.Name == "default_profile" && defaultID == 0 {
			defaultID = p.ID
		}
	}
	if defaultID == 0 {
		bestID := 0
		for id := range next {
			if bestID == 0 || id < bestID {
				bestID = id
			}
		}
		defaultID = bestID
	}
	abilityRuntimeMu.Lock()
	profilesByID = next
	defaultProfileID = defaultID
	abilityRuntimeMu.Unlock()
}

// SetNPCProfileBindings replaces spawn/archetype to profile mappings.
func SetNPCProfileBindings(bindings []NPCProfileBinding) {
	bySpawn := make(map[int]NPCProfileBinding)
	byActor := make(map[int]NPCProfileBinding)
	for _, b := range bindings {
		if b.ProfileID <= 0 || !b.Enabled {
			continue
		}
		if b.NPCSpawnID > 0 {
			bySpawn[b.NPCSpawnID] = b
		}
		if b.ActorDefID > 0 {
			byActor[b.ActorDefID] = b
		}
	}
	abilityRuntimeMu.Lock()
	profileBindingBySpawn = bySpawn
	profileBindingByActorID = byActor
	abilityRuntimeMu.Unlock()
}

func resolveNPCCombatProfile(spawnID, actorDefID int) NPCCombatProfile {
	abilityRuntimeMu.RLock()
	defer abilityRuntimeMu.RUnlock()
	resolveProfile := func(profileID int) (NPCCombatProfile, bool) {
		if profileID <= 0 {
			return NPCCombatProfile{}, false
		}
		p, ok := profilesByID[profileID]
		if !ok || !p.Enabled {
			return NPCCombatProfile{}, false
		}
		return p, true
	}
	if spawnID > 0 {
		if b, ok := profileBindingBySpawn[spawnID]; ok {
			if p, ok := resolveProfile(b.ProfileID); ok {
				return p
			}
		}
	}
	if actorDefID > 0 {
		if b, ok := profileBindingByActorID[actorDefID]; ok {
			if p, ok := resolveProfile(b.ProfileID); ok {
				return p
			}
		}
	}
	if p, ok := resolveProfile(defaultProfileID); ok {
		return p
	}
	return defaultNPCCombatProfile
}

func resolveNPCGlobalGCDMs(npc *Actor) int64 {
	if npc == nil || !npc.IsNPC {
		return defaultNPCCombatProfile.GlobalGCDMs
	}
	profile := resolveNPCCombatProfile(npc.SpawnID, npc.ActorDefID)
	if profile.GlobalGCDMs < 0 {
		return 0
	}
	return profile.GlobalGCDMs
}

func consumeNPCAbilityDecisionBudget(npc *Actor, now int64) bool {
	if npc == nil || !npc.IsNPC {
		return true
	}
	profile := resolveNPCCombatProfile(npc.SpawnID, npc.ActorDefID)
	tickMs := profile.DecisionTickMs
	if tickMs < 0 {
		tickMs = 0
	}
	npc.Mu.Lock()
	last := npc.LastAbilityDecisionAt
	if tickMs > 0 && now-last < tickMs {
		npc.Mu.Unlock()
		return false
	}
	npc.LastAbilityDecisionAt = now
	npc.Mu.Unlock()
	return true
}

func resolveNPCSpecialChainPolicy(npc *Actor) (maxConsecutive int, resetMs int64) {
	if npc == nil || !npc.IsNPC {
		return 1, 1200
	}
	profile := resolveNPCCombatProfile(npc.SpawnID, npc.ActorDefID)
	maxConsecutive = profile.MaxConsecutiveSpecials
	if maxConsecutive < 1 {
		maxConsecutive = 1
	}
	if !profile.AllowChainCast {
		maxConsecutive = 1
	}
	resetMs = profile.GlobalGCDMs * 2
	if resetMs < 1200 {
		resetMs = 1200
	}
	return maxConsecutive, resetMs
}

func canNPCStartSpecialChain(npc *Actor, now int64) bool {
	if npc == nil || !npc.IsNPC {
		return true
	}
	maxConsecutive, resetMs := resolveNPCSpecialChainPolicy(npc)
	npc.Mu.Lock()
	defer npc.Mu.Unlock()

	lastSpecialAt := npc.LastSpecialAt
	if lastSpecialAt <= 0 || now-lastSpecialAt > resetMs {
		npc.SpecialChainCount = 0
		return true
	}
	if npc.SpecialChainCount < 0 {
		npc.SpecialChainCount = 0
	}
	return npc.SpecialChainCount < maxConsecutive
}

func markNPCSpecialChainCastStarted(npc *Actor, now int64, previousLastSpecialAt int64) {
	if npc == nil || !npc.IsNPC {
		return
	}
	maxConsecutive, resetMs := resolveNPCSpecialChainPolicy(npc)
	if previousLastSpecialAt <= 0 || now-previousLastSpecialAt > resetMs {
		npc.SpecialChainCount = 1
		return
	}
	if npc.SpecialChainCount < 1 {
		npc.SpecialChainCount = 1
	} else {
		npc.SpecialChainCount++
	}
	if npc.SpecialChainCount > maxConsecutive {
		npc.SpecialChainCount = maxConsecutive
	}
}

func breakNPCSpecialChain(npc *Actor) {
	if npc == nil || !npc.IsNPC {
		return
	}
	npc.Mu.Lock()
	npc.SpecialChainCount = 0
	npc.Mu.Unlock()
}

// GetNPCAbilityLoadoutByRID resolves the effective loadout rows for one NPC.
// Returns a copy of rows, or nil if NPC/runtime lookup fails.
func GetNPCAbilityLoadoutByRID(w *World, npcRID uint32) []NPCAbilityLoadoutEntry {
	if w == nil || npcRID == 0 {
		return nil
	}
	npc, _ := w.FindActor(npcRID)
	if npc == nil || !npc.IsNPC {
		return nil
	}
	return resolveNPCAbilityLoadout(npc.SpawnID, npc.ActorDefID)
}

// GetNPCAbilityDecisionContextByRID returns the same normalized context that
// the runtime uses for phase_tag / condition_lua evaluation.
func GetNPCAbilityDecisionContextByRID(w *World, npcRID, targetRID uint32) (NPCAbilityDecisionContext, bool) {
	if w == nil || npcRID == 0 || targetRID == 0 {
		return NPCAbilityDecisionContext{}, false
	}
	npc, area := w.FindActor(npcRID)
	if npc == nil || area == nil || !npc.IsNPC {
		return NPCAbilityDecisionContext{}, false
	}
	target, ok := area.GetActor(targetRID)
	if !ok || target == nil {
		return NPCAbilityDecisionContext{}, false
	}
	ctx := buildLoadoutEvalContext(npc, target)
	return NPCAbilityDecisionContext{
		Distance:    ctx.distance,
		NPCHPPct:    ctx.npcHPPct,
		TargetHPPct: ctx.targetHPPct,
		NPCSPPct:    ctx.npcSPPct,
		TargetSPPct: ctx.targetSPPct,
		NPCMPPct:    ctx.npcMPPct,
		TargetMPPct: ctx.targetMPPct,
		PhaseTag:    ctx.phaseTag,
		Phase:       int(ctx.phaseIndex),
	}, true
}

// SetNPCDecisionHook registers an optional callback invoked during NPC chase
// ticks before the built-in loadout selector runs. Scripts can use this hook
// to call NPCCombat.try_cast and drive encounter logic.
func SetNPCDecisionHook(hook func(area *Area, npc *Actor, target *Actor, now int64) bool) {
	abilityRuntimeMu.Lock()
	npcDecisionHook = hook
	abilityRuntimeMu.Unlock()
}

func runNPCDecisionHook(area *Area, npc *Actor, target *Actor, now int64) bool {
	abilityRuntimeMu.RLock()
	hook := npcDecisionHook
	abilityRuntimeMu.RUnlock()
	if hook == nil {
		return false
	}
	return hook(area, npc, target, now)
}
