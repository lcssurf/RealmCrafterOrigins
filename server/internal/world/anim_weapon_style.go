// Package world - anim_weapon_style.go
//
// Central place for the "{BaseAction}_{WeaponAnimStyle}" name composition —
// previously duplicated inline in combat_basic.go (BroadcastAttack's
// attackAction) and net/client.go (the two equip-change handlers, before
// Actor.IdleAction() existed). Any code that broadcasts a character
// animation and wants it to reflect the equipped weapon's PHYSICAL
// grip/pose archetype (item_templates.weapon_anim_style — "sword_1h",
// "bow", "staff"...) should call ComposeWeaponAction instead of building
// the string by hand, so the one fallback rule lives in one place.
package world

// ComposeWeaponAction returns baseAction composed with actor's equipped
// weapon style ("Attack" -> "Attack_sword_1h"), or baseAction unchanged
// when the actor has no style configured (BasicAttackAnimStyle == "" —
// every weapon predates the weapon_anim_style column, and most still won't
// have one set, so this is zero-behavior-change for anyone who hasn't
// configured a style yet).
//
// This does NOT resolve the composite name against the actor's own
// animation bindings or the anim_vocabulary fallback tree — that happens
// downstream in BroadcastAnimate (combat_events.go), which walks
// anim_vocabulary's parent_id chain and, as a last resort, the textual
// prefix-cut heuristic when the composite was never registered as an
// explicit vocabulary node. ComposeWeaponAction only builds the NAME;
// resolving whether a binding actually exists for it is BroadcastAnimate's
// job, not this function's.
func ComposeWeaponAction(actor *Actor, baseAction string) string {
	if actor == nil || actor.BasicAttackAnimStyle == "" {
		return baseAction
	}
	return baseAction + "_" + actor.BasicAttackAnimStyle
}
