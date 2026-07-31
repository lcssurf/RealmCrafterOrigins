#pragma once
#include "rco/physics/character_physics.h"
#include "rco/physics/collision_world.h"
#include <glm/glm.hpp>
#include <optional>

struct GLFWwindow;
namespace rco { struct PlayerState; }

namespace rco {

// Handles all local-player movement: keyboard walking, click-to-move,
// sprint, auto-run, turn-to-face, and combat auto-approach INPUT — the
// actual physics resolution (gravity, collision, ground detection, slope,
// step-up) is delegated entirely to rco::physics::CharacterPhysics's
// unified capsule sweep (see docs/TECH_DEBT.md #125's architecture
// investigation). This class no longer implements any collision/gravity
// logic itself — it builds a CapsuleMoveInput from resolved input and
// applies the CapsuleMoveResult back to PlayerState.
//
// Call Update() once per frame after camera/mouse RMB yaw sync and before
// anything else reads player position/on-ground state for this frame.
class PlayerController {
public:
    struct Config {
        float speed               = 8.0f;
        float back_mult           = 0.65f;
        float sprint_mult         = 1.65f;  // speed multiplier while Shift is held
        // Yaw smoothing rate for turn-to-face-movement (SmoothLerpFactor's
        // exponential rate_per_sec, see player_controller.cpp).
        float turn_rate           = 180.0f;
        float click_stop_radius   = 0.08f;
        float min_move_len_sq     = 1e-8f;
        float min_proj_len        = 0.001f;
        float min_dir_len_sq      = 0.001f;
    };

    struct Result {
        bool  sprinting     = false;
        bool  auto_running  = false;
        // Set true for exactly the frame auto-approach (see approach_requested
        // below) reaches AttackRange of the target — the caller (main.cpp) owns
        // the approach_requested latch and must clear it on this signal, or the
        // next Update() call would still pass approach_requested=true and the
        // approach would stay "active" (harmlessly inert while in range, but
        // wrong: a target that later kites away would silently resume the
        // approach without a fresh explicit press).
        bool  approach_arrived         = false;
        // Set true for exactly the frame WASD/click-to-move input preempted an
        // in-progress approach (has_manual_input was true while
        // approach_requested was also true). Movement-input priority over
        // approach was already in place; this just surfaces that outcome so
        // the caller can clear its latch — approach must not resume on its own
        // once WASD lets go, only via another explicit approach_requested=true.
        bool  approach_cancelled_by_input = false;
    };

    void      SetMoveTarget(const glm::vec3& t) { move_target_ = t; has_move_target_ = true; }
    void      CancelMoveTarget()               { has_move_target_ = false; }
    bool      HasMoveTarget()            const { return has_move_target_; }
    glm::vec3 MoveTarget()               const { return move_target_; }

    void Reset() { physics_.Reset(); auto_run_ = false; CancelMoveTarget(); }
    bool IsOnGround() const { return physics_.IsOnGround(); }
    bool IsAutoRunning() const { return auto_run_; }
    const Config& GetConfig() const { return cfg_; }
    void SetConfig(const Config& cfg) { cfg_ = cfg; }
    // Physics tuning (gravity/jump/slope/step-up/etc — see
    // physics::CharacterPhysics::Config) lives on the physics sub-object
    // directly, not duplicated here.
    const physics::CharacterPhysics::Config& GetPhysicsConfig() const { return physics_.GetConfig(); }
    void SetPhysicsConfig(const physics::CharacterPhysics::Config& cfg) { physics_.SetConfig(cfg); }

    // Action-mode controller (RCO): WASD is relative to the CAMERA
    // (camera_yaw, degrees — same convention as PlayerState::yaw/
    // Camera::GetYaw()), and the player smoothly turns to face the
    // resulting movement direction instead of strafing with a fixed
    // orientation (see player_controller.cpp's turn-to-face block). Caller
    // (main.cpp) no longer syncs player.yaw = camera.GetYaw() itself —
    // camera_yaw is passed in purely as the movement-direction basis, and
    // player.yaw is now owned/smoothed by this class.
    //
    // is_attacking/target_pos: while the player's Attack animation is
    // playing AND a valid combat target is resolved (caller passes
    // target_pos = world_actors.find(combat_target)'s position, or
    // std::nullopt if no target/target not found), the turn-to-face block
    // targets the ENEMY instead of the movement direction — even while
    // standing still (dir==0). Movement itself is NEVER gated by
    // is_attacking here: WASD keeps moving the player normally (camera-
    // relative strafe) while the body independently faces the target, by
    // design (no attack-time movement lock exists anywhere else in this
    // class either). Defaults (false/nullopt) preserve today's plain
    // movement-facing behavior for any caller that doesn't pass them.
    //
    // Auto-approach (walking toward an out-of-range target) is gated on
    // approach_requested, NOT target_pos alone and NOT is_attacking. This is
    // a caller-owned latch: main.cpp sets it true only inside its explicit
    // Attack-button handler (the click/keypress, never the passive 0.85s
    // auto-attack resend), and clears it in response to Result::approach_arrived
    // or Result::approach_cancelled_by_input, or when the target itself is
    // lost/deselected. PlayerController does not persist this state itself —
    // passing approach_requested=false (the default) always disables approach,
    // same as if no target were selected at all. Turn-to-face stays gated on
    // is_attacking (see above): it's deliberately an attack-time-only
    // behavior, unrelated to approach_requested.
    //
    // world: everything the underlying CharacterPhysics::Move() needs to
    // resolve collision/ground this frame (col_data, dynamic boxes, terrain
    // height/normal callbacks) — see rco::physics::CollisionWorld. Caller
    // should rebuild it (dynamic boxes especially) for THIS frame before
    // calling Update() — see docs/TECH_DEBT.md #125 on why a stale list
    // mattered more than expected once.
    //
    // external_move_delta: when present, OVERRIDES WASD/click-to-move/turn-
    // to-face entirely for this call — the caller (main.cpp) already owns
    // the direction/facing/duration of whatever's driving this (today: the
    // dodge roll's per-frame impulse, computed from its own timing curve).
    // Still resolved through this SAME CharacterPhysics instance/Move()
    // call as ordinary movement, so it collides with walls/mesh/terrain —
    // slides on a glancing hit, stops square-on — instead of a raw,
    // uncollided position write. Jump is also suppressed while overriding
    // (a dodge shouldn't also trigger a jump). See docs/TECH_DEBT.md #125.
    Result Update(GLFWwindow* win, float dt, bool dead,
                  PlayerState& player,
                  const physics::CollisionWorld& world,
                  float camera_yaw,
                  bool rmb_held, bool lmb_held,
                  bool is_attacking = false,
                  std::optional<glm::vec3> target_pos = std::nullopt,
                  bool approach_requested = false,
                  std::optional<glm::vec2> external_move_delta = std::nullopt);

private:
    Config                      cfg_{};
    physics::CharacterPhysics   physics_{};
    bool      auto_run_        = false;
    glm::vec3 move_target_     = {};
    bool      has_move_target_ = false;
    bool      numlock_prev_    = false;
};

} // namespace rco
