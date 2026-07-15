#pragma once
#include <glm/glm.hpp>
#include <optional>

struct GLFWwindow;
namespace rco { struct PlayerState; }
namespace rco::renderer { class Terrain; }

namespace rco {

// Handles all local-player movement: keyboard walking, click-to-move,
// gravity, slope blocking, slide, jumping, sprint, and auto-run.
// Call Update() once per frame after camera/mouse RMB yaw sync and
// before collision resolution.
class PlayerController {
public:
    struct Config {
        float speed               = 8.0f;
        float back_mult           = 0.65f;
        float sprint_mult         = 1.65f;  // speed multiplier while Shift is held
        // Yaw smoothing rate for turn-to-face-movement (SmoothLerpFactor's
        // exponential rate_per_sec, see player_controller.cpp) — was
        // declared but never read (dead field); now active. Bumped from
        // the old dead default (150.0f) to 180.0f, the "fast but not
        // instant" ballpark requested for LOCAL input response — notably
        // higher than the 20.0f used for remote-actor network-catchup
        // smoothing (main.cpp's kYawLerpRate), since local input needs to
        // feel responsive, not just gradually correct a network delta.
        float turn_rate           = 180.0f;
        float max_slope_deg       = 45.0f;
        float jump_vel            = 9.0f;
        float gravity             = 20.0f;
        float snap_down           = 0.8f;
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

    void Reset() { vel_y_ = 0.f; on_ground_ = true; auto_run_ = false; CancelMoveTarget(); }
    bool IsOnGround() const { return on_ground_; }
    bool IsAutoRunning() const { return auto_run_; }
    const Config& GetConfig() const { return cfg_; }
    void SetConfig(const Config& cfg) { cfg_ = cfg; }

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
    Result Update(GLFWwindow* win, float dt, bool dead,
                  PlayerState& player,
                  const renderer::Terrain& terrain,
                  float camera_yaw,
                  bool rmb_held, bool lmb_held,
                  bool is_attacking = false,
                  std::optional<glm::vec3> target_pos = std::nullopt,
                  bool approach_requested = false);

private:
    Config    cfg_{};
    float     vel_y_          = 0.f;
    bool      on_ground_      = true;
    bool      auto_run_       = false;
    glm::vec3 move_target_    = {};
    bool      has_move_target_= false;
    bool      numlock_prev_   = false;

    bool ApplyHorizontalMove(glm::vec2 delta, PlayerState& player,
                              const renderer::Terrain& terrain);
    void UpdateVertical(float dt, PlayerState& player,
                        const renderer::Terrain& terrain);
};

} // namespace rco
