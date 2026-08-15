#pragma once
#include <cstdint>
#include <glm/glm.hpp>

namespace rco::gameplay {

// Centralizes basic-attack combat intent: target selection, range/movement
// gating, the out-of-range cancel timeout, and auto-attack send cadence.
//
// This used to be spread across several independent call sites in main.cpp
// (explicit-click immediate-attack handler, passive auto-attack resend loop,
// out-of-range timeout block, plus ~10 read/write sites for combat_target
// and combat_approach_active scattered through target selection, Tab-cycle,
// Esc, death/area-transition cleanup, the target ring, and spellbar range
// checks). Each was edited in a different round of the same day's work,
// which is exactly how the movement-pause regression happened: a
// `player_is_moving` check got re-declared locally in the auto-attack block
// AFTER main.cpp's `last_player_pos` had already been overwritten to the
// CURRENT frame's position by an unrelated per-frame block (water ripple
// stamping, ~main.cpp:6854), making the delta always ~0 and the pause check
// a permanent no-op. Movement detection here is now self-contained — it
// tracks its own previous-position snapshot (prev_player_pos_), updated
// exactly once per frame inside Update(), so no other system's use of
// position tracking can silently invalidate it again.
//
// Target ACQUISITION (screen-space raycast picking, Tab-cycle nearest-hostile
// scan) intentionally stays in main.cpp — that's input/rendering logic, not
// combat decision logic. This controller owns what happens once a target
// (rid) is known: is it in range, is the player allowed to swing right now,
// has it been out of range too long. main.cpp calls SetTarget()/ClearTarget()
// when it resolves a new target or wants to drop the current one, and calls
// Update() exactly once per frame to get this frame's decision.
class CombatIntentController {
public:
    // World-units/frame delta below which the player is considered
    // stationary — same threshold used everywhere else in the client for
    // this (Walk/Run locomotion, ripple stamp gating).
    static constexpr float kMoveThreshold = 0.02f;

    // Basic-attack cadence, mirrored from server/internal/world/combat.go's
    // CombatDelay. MUST stay in sync with the server — same contract the
    // pre-modularization code already carried at this constant's old
    // definition site (main.cpp).
    static constexpr float kCombatDelayMs = 800.0f;

    // Player stands with a target selected but out of range, doing nothing
    // about it (no auto-approach in flight) for longer than this: the
    // intent is dropped and a fresh click is required. See ClearTarget().
    static constexpr double kOutOfRangeTimeoutSec = 5.0;

    struct Input {
        double    now              = 0.0;   // glfwGetTime() seconds
        glm::vec2 player_pos{0.f};           // XZ
        bool      player_dead      = false;
        bool      conn_connected   = false;
        // skill_loadout_open || local_guarding || dodge_roll_active — any
        // state that should suppress sending an attack packet this frame.
        bool      blocked          = false;
        bool      target_exists    = false;  // target() still present in world_actors
        glm::vec2 target_pos{0.f};           // XZ, valid only if target_exists
        float     attack_range     = 0.f;    // player.derived.AttackRange
        float     attack_speed_mult = 1.f;   // player.derived.AttackSpeedMult
        // This frame's explicit Attack-button press already confirmed
        // against the CURRENT target (main.cpp's this_click_confirmed_
        // combat_target) — distinct from a stale target left over from an
        // earlier click. False on every frame that isn't a fresh confirmed
        // click.
        bool      explicit_attack_click = false;
    };

    struct Result {
        // Caller should send kPAttackActor(target()) this frame.
        bool send_attack_packet = false;
        // Informational only — target() was force-cleared this call by the
        // out-of-range timeout (same cleanup Esc/target-switch already do).
        bool target_cleared_by_timeout = false;
    };

    uint32_t Target() const { return target_; }
    bool     ApproachActive() const { return approach_active_; }

    // New target selected (raycast hit, Tab-cycle). Resets the out-of-range
    // timer and the approach latch — a fresh target starts clean, same as
    // the pre-modularization combat_target=rid assignment did (it never
    // touched last_attack_sent, so neither does this: re-targeting doesn't
    // reset the attack-cadence timer, preserving "switching targets mid-
    // cooldown doesn't grant a free early swing").
    void SetTarget(uint32_t rid) {
        target_ = rid;
        approach_active_ = false;
        out_of_range_since_ = 0.0;
    }

    // Drop the current target entirely. Same cleanup every one of the old
    // scattered call sites performed (Esc, dialog-NPC click, target death,
    // area transition, player death, target gone from world_actors, out-of-
    // range timeout).
    void ClearTarget() {
        target_ = 0;
        approach_active_ = false;
        out_of_range_since_ = 0.0;
    }

    // Only call site that should ever set this true is main.cpp's explicit-
    // click handler going through Update() below (explicit_attack_click +
    // out of range). Exposed as a setter only so PlayerController's report
    // (approach_arrived / approach_cancelled_by_input) can clear it — mirrors
    // the old file-owned-sticky-latch comment on combat_approach_active.
    void SetApproachActive(bool v) { approach_active_ = v; }

    // Called exactly once per frame. Encapsulates: movement-pause,
    // in-range check, explicit-click immediate attack (or approach-latch
    // arming when out of range), the out-of-range cancel timeout, and the
    // passive auto-attack resend cadence.
    Result Update(const Input& in) {
        Result r;

        bool player_is_moving = false;
        if (have_prev_pos_) {
            player_is_moving = glm::length(in.player_pos - prev_player_pos_) > kMoveThreshold;
        }
        prev_player_pos_ = in.player_pos;
        have_prev_pos_   = true;

        if (target_ == 0) {
            out_of_range_since_ = 0.0;
            return r;
        }
        if (!in.target_exists) {
            // Caller is responsible for ClearTarget() once it notices the
            // target left world_actors (main.cpp still does that at the
            // target-ring site) — Update() just refuses to act on stale
            // position data this frame rather than assuming a range.
            return r;
        }

        glm::vec2 diff = in.target_pos - in.player_pos;
        bool in_range = in.attack_range > 0.f &&
            glm::dot(diff, diff) <= in.attack_range * in.attack_range;

        if (in.explicit_attack_click && !in.blocked && !in.player_dead && in.conn_connected) {
            if (in_range) {
                if (!player_is_moving) {
                    r.send_attack_packet = true;
                    last_attack_sent_ = in.now;
                }
                // else: pause-not-cancel — an explicit click doesn't force a
                // send through movement either, same as the passive resend.
            } else {
                // Out of range: this explicit Attack press is what turns
                // approach on — never the passive resend below, and never
                // mere target selection.
                approach_active_ = true;
            }
        }

        if (in_range || approach_active_) {
            // In range, or the auto-approach latch is actively trying to
            // close the distance — neither should accumulate toward the
            // timeout. If approach later gets cancelled by manual WASD
            // input while still out of range, the timer starts fresh from
            // that moment (falls through to the else branch on a later
            // call), not from whenever the player first went out of range.
            out_of_range_since_ = 0.0;
        } else {
            if (out_of_range_since_ <= 0.0) {
                out_of_range_since_ = in.now;
            } else if (in.now - out_of_range_since_ >= kOutOfRangeTimeoutSec) {
                ClearTarget();
                r.target_cleared_by_timeout = true;
                return r;
            }
        }

        // Passive auto-attack resend, at the SAME effective cadence the
        // server's cooldown gate actually enforces (combat_basic.go:42-54),
        // not a fixed interval.
        if (!r.send_attack_packet && !in.player_dead && in.conn_connected && !in.blocked
            && !player_is_moving && in_range) {
            double atk_speed = in.attack_speed_mult > 0.f
                ? static_cast<double>(in.attack_speed_mult) : 1.0;
            double effective_interval_sec = (static_cast<double>(kCombatDelayMs) / 1000.0) / atk_speed;
            if (in.now - last_attack_sent_ >= effective_interval_sec) {
                r.send_attack_packet = true;
                last_attack_sent_ = in.now;
            }
        }

        return r;
    }

private:
    uint32_t  target_             = 0;
    bool      approach_active_    = false;
    double    out_of_range_since_ = 0.0;
    double    last_attack_sent_   = 0.0;

    bool      have_prev_pos_   = false;
    glm::vec2 prev_player_pos_{0.f};
};

} // namespace rco::gameplay
