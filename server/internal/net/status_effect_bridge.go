package net

import (
	"log"

	"realm-crafter/server/internal/protocol"
	"realm-crafter/server/internal/world"
)

// handleStatusEffectBroadcast serializes and sends PStatusEffectDelta — the
// packet ID (131) has existed since before this round but was never
// actually built/sent by anything (confirmed dead wire, see
// docs/TECH_DEBT.md, Buffs/Debuffs/CC investigation, item 1). This is the
// real implementation, registered as world.SetStatusEffectHook in
// server.go, mirroring handleAbilityFXBroadcast/handleBloodFXBroadcast
// (combat_fx_bridge.go)'s "world package owns WHEN, net package owns HOW
// to encode" split.
//
// durationMs is RELATIVE (ms remaining, not an absolute epoch timestamp —
// see StatusEffectHook doc, world/status_effects.go) — this Writer has no
// 64-bit integer writer, and a relative duration lets the client stamp its
// own local receipt time and count down locally, same pattern as
// SkillHotbar's cooldown state.
//
// Format: target_rid(u32) + template_id(u32) + kind(str) + cc_type(str) +
// duration_ms(u32) + icon_path(str) + applied(u8, 1=apply/refresh, 0=expire)
func (s *Server) handleStatusEffectBroadcast(
	area *world.Area,
	targetRID uint32,
	templateID int,
	kind, ccType string,
	durationMs int64,
	iconPath string,
	applied bool,
) {
	if s == nil || area == nil || targetRID == 0 {
		return
	}
	if durationMs < 0 {
		durationMs = 0
	}

	var w Writer
	w.WriteUint32(targetRID)
	w.WriteUint32(uint32(templateID))
	w.WriteString(kind)
	w.WriteString(ccType)
	w.WriteUint32(uint32(durationMs))
	w.WriteString(iconPath)
	w.WriteBool(applied)

	area.BroadcastAll(buildFramedPacket(protocol.PStatusEffectDelta, w.Bytes()))

	// Fase 2 (Buffs/Debuffs/CC) fix: a Buff/Debuff mutates the target's
	// cached actor.Derived (world.RecomputeDerivedStatsFast, called from
	// TryApplyStatMod/tickStatusEffects — status_effects.go/area.go) on
	// BOTH apply (applied=true) and expiry (applied=false), since either
	// transition changes which StatMods are active. Without this, the
	// client's own MovementSpeedMult/etc copy (last synced at login/equip-
	// change via PFullStats) goes stale until the next unrelated
	// full-stats resend — confirmed root cause of "buff sem efeito
	// perceptível" (docs/TECH_DEBT.md, Buffs/Debuffs/CC investigation).
	// CC (Kind=="cc") never touches Derived (IsStunned/IsRooted/
	// SlowMultiplier all re-scan ActiveEffects live — see actor.go), so it
	// deliberately does NOT resend here — would be a wasted packet.
	if kind == string(world.StatusKindBuff) || kind == string(world.StatusKindDebuff) {
		if targetActor, ok := area.GetActor(targetRID); ok {
			// TEMP DIAG (Buffs/Debuffs/CC — "MovementSpeedMult não muda"
			// investigation): confirms this branch actually runs, and what
			// MovementSpeedMult looks like on targetActor.Derived at the
			// exact moment PFullStats is about to be built/sent.
			targetActor.Mu.Lock()
			mult := targetActor.Derived.MovementSpeedMult
			targetActor.Mu.Unlock()
			log.Printf("[statmod-diag] handleStatusEffectBroadcast: resending PFullStats target=%d kind=%s applied=%v MovementSpeedMult=%.4f",
				targetRID, kind, applied, mult)
			sendFullStatsToActor(targetActor)
		} else {
			log.Printf("[statmod-diag] handleStatusEffectBroadcast: kind=%s applied=%v but GetActor(%d) failed — no resend",
				kind, applied, targetRID)
		}
	}
}
