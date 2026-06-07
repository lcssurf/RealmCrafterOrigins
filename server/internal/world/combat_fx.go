package world

import "sync"

type FXPhase string

const (
	FXPhaseWindup FXPhase = "windup"
	FXPhaseImpact FXPhase = "impact"
)

// AbilityFXHook is implemented by the net layer to serialize and broadcast
// ability VFX/SFX payloads to connected clients.
type AbilityFXHook func(
	area *Area,
	casterRID, targetRID uint32,
	abilityID uint32,
	vfxPath, sfxPath string,
	posX, posY, posZ float32,
	magnitude float32,
	phase string,
)

var (
	abilityFXHookMu sync.RWMutex
	abilityFXHook   AbilityFXHook
)

func SetAbilityFXHook(h AbilityFXHook) {
	abilityFXHookMu.Lock()
	abilityFXHook = h
	abilityFXHookMu.Unlock()
}

// BroadcastAbilityFX dispatches windup/impact VFX/SFX metadata for one ability
// event. Actual packet serialization/broadcast is owned by the net hook.
func BroadcastAbilityFX(area *Area, caster, target *Actor, ability AbilityTemplate, phase FXPhase) {
	abilityFXHookMu.RLock()
	h := abilityFXHook
	abilityFXHookMu.RUnlock()
	if h == nil || caster == nil || area == nil {
		return
	}

	var vfxPath, sfxPath string
	switch phase {
	case FXPhaseWindup:
		vfxPath = ability.VFXPathWindup
		sfxPath = ability.SFXPathWindup
	case FXPhaseImpact:
		vfxPath = ability.VFXPathImpact
		sfxPath = ability.SFXPathImpact
	default:
		return
	}
	if vfxPath == "" && sfxPath == "" {
		return
	}

	var posX, posY, posZ float32
	if phase == FXPhaseImpact && target != nil {
		target.Mu.Lock()
		posX, posY, posZ = target.X, target.Y, target.Z
		target.Mu.Unlock()
	} else {
		caster.Mu.Lock()
		posX, posY, posZ = caster.X, caster.Y, caster.Z
		caster.Mu.Unlock()
	}

	casterRID := caster.RuntimeID
	targetRID := uint32(0)
	if target != nil {
		targetRID = target.RuntimeID
	}
	abilityID := uint32(0)
	if ability.ID > 0 {
		abilityID = uint32(ability.ID)
	}

	h(
		area,
		casterRID, targetRID,
		abilityID,
		vfxPath, sfxPath,
		posX, posY, posZ,
		1.0,
		string(phase),
	)
}
