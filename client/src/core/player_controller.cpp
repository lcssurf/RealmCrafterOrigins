#include "player_controller.h"
#include "../ui/game_state.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace rco {
namespace {
// Shortest-arc yaw smoothing — SAME pattern already active in production
// for remote-actor yaw interpolation (main.cpp:102-117, applied at
// main.cpp:3887-3911 with kYawLerpRate=20.0f for network catch-up).
// Duplicated here (not shared via a header) since main.cpp's versions are
// file-local `static` — this is a small, self-contained reapplication of
// the same math, not a new mechanism.
float NormalizeYawDegrees(float yaw) {
    while (yaw < 0.f) yaw += 360.f;
    while (yaw >= 360.f) yaw -= 360.f;
    return yaw;
}

float ShortestYawDeltaDegrees(float from, float to) {
    return std::fmod((to - from) + 540.f, 360.f) - 180.f;
}

float SmoothLerpFactor(float dt, float rate_per_sec) {
    if (dt <= 0.f || rate_per_sec <= 0.f) return 1.f;
    return std::clamp(1.f - std::exp(-rate_per_sec * dt), 0.f, 1.f);
}
}

// ---------------------------------------------------------------------------
// Update
//
// This class now ONLY resolves INPUT into a desired movement — WASD/click-
// to-move direction, sprint, auto-run, turn-to-face, auto-approach, jump
// intent. The actual collision/gravity/ground/slope/step-up resolution is
// entirely rco::physics::CharacterPhysics::Move()'s job (see
// docs/TECH_DEBT.md #125's architecture investigation) — this function
// builds ONE CapsuleMoveInput from whatever input won this frame (WASD XOR
// click-to-move, same mutual-exclusion as before) and applies the single
// CapsuleMoveResult back to PlayerState at the end. No more
// ApplyHorizontalMove/UpdateVertical/ColData::Resolve/ComputeGroundHeight
// calls anywhere in this file.
// ---------------------------------------------------------------------------
PlayerController::Result PlayerController::Update(
        GLFWwindow* win, float dt, bool dead,
        PlayerState& player,
        const physics::CollisionWorld& world,
        float camera_yaw,
        bool rmb_held, bool lmb_held,
        bool is_attacking,
        std::optional<glm::vec3> target_pos,
        bool approach_requested,
        std::optional<glm::vec2> external_move_delta)
{
    // has_target: gates turn-to-face-target (used further below) — stays
    // tied to is_attacking on purpose. It's specifically for the "body
    // faces the enemy during the Attack anim, even standing still" case;
    // widening it to fire on mere target-selection would turn every target
    // click into an instant facing-lock, fighting WASD strafe/kiting before
    // any attack is even happening.
    const bool has_target = is_attacking && target_pos.has_value();
    Result r{};

    if (dead) {
        player.y = world.sample_terrain_height(player.x, player.z);
        physics_.Reset();
        auto_run_ = false;
        CancelMoveTarget();
        return r;
    }

    // desired_delta_2d/jump_requested: the ONE horizontal movement (and
    // jump intent) this frame, handed to CharacterPhysics::Move() at the
    // end.
    //
    // external_move_delta (dodge roll) is ADDITIVE with WASD, not a
    // replacement for it (see docs/TECH_DEBT.md #128, "dash não se sente
    // como dash" investigation — confirmed by the dev: dodge is always
    // triggered while holding WASD, and the pre-reformulation dash summed
    // its own velocity on top of the current walk velocity; making dodge
    // mutually-exclusive with WASD during the reformulation silently
    // dropped that composition, which is what made the dash feel like
    // plain walking despite the raw dash distance itself being correct).
    // The dash's DIRECTION still comes from wherever it was captured once,
    // at roll-start (main.cpp) — only its magnitude combines with
    // whatever WASD velocity is live this frame, every frame, until it
    // decays out. Click-to-move, turn-to-face and auto-approach are still
    // skipped entirely while a dodge is active (see the `dodge_active`
    // gates below) — the dash owns facing/target-approach, same as before.
    glm::vec2 desired_delta_2d(0.f);
    bool jump_requested = false;
    const bool dodge_active = external_move_delta.has_value();

    // --- NumLock: toggle auto-run ---
    {
        bool nl = glfwGetKey(win, GLFW_KEY_NUM_LOCK) == GLFW_PRESS;
        if (nl && !numlock_prev_) auto_run_ = !auto_run_;
        numlock_prev_ = nl;
    }
    r.auto_running = auto_run_;

    // --- Sprint (Shift) ---
    bool sprinting = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                     glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    r.sprinting = sprinting;

    bool both_held = rmb_held && lmb_held;
    bool any_key_move = false;

    // WASD basis is relative to the CAMERA now, not the player's own
    // (previously camera-locked) yaw — see camera_yaw param doc (header).
    // This is what makes A/D turn INTO a strafe-relative-to-view direction
    // instead of a strafe-relative-to-wherever-the-body-happens-to-face.
    float yr = glm::radians(camera_yaw);
    glm::vec2 fdir = {-std::sin(yr), -std::cos(yr)};
    glm::vec2 rdir = { std::cos(yr), -std::sin(yr)};

    // --- Build movement direction (Action mode only) ---
    bool moving_fwd = glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS || both_held || auto_run_;
    bool moving_back = glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS;

    // Any explicit movement input cancels auto-run.
    if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS ||
        glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS ||
        glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS ||
        glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS ||
        glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS ||
        glfwGetKey(win, GLFW_KEY_E) == GLFW_PRESS) {
        auto_run_ = false;
    }

    bool strafe_l = glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS ||
                    glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS;
    bool strafe_r = glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS ||
                    glfwGetKey(win, GLFW_KEY_E) == GLFW_PRESS;

    glm::vec2 dir(0.f);
    if (moving_fwd) dir += fdir;
    if (moving_back) dir -= fdir;
    if (strafe_l) dir -= rdir;
    if (strafe_r) dir += rdir;

    // Auto-approach: WASD has ABSOLUTE priority — this block doesn't even
    // evaluate distance/range when the player has any manual movement
    // input, let alone blend/add into `dir`. has_manual_input is a
    // snapshot of the WASD-only direction BEFORE any override, so there's
    // no ambiguity about which source wins. Gated on the caller-owned
    // approach_requested latch (see header doc) — never on mere target
    // selection — so approach only ever starts from an explicit Attack-
    // button press, not passively from having a target. Skipped entirely
    // during a dodge (`!dodge_active`): the dash must never get redirected
    // toward a target mid-roll, and approach shouldn't silently keep
    // steering underneath it either — same reasoning as turn-to-face below.
    bool has_manual_input = glm::dot(dir, dir) > cfg_.min_dir_len_sq;
    if (!dodge_active && approach_requested && target_pos.has_value()) {
        if (has_manual_input) {
            // WASD preempted an in-progress approach this frame. Surface it
            // so the caller clears approach_requested for good — approach
            // must not silently resume once the player lets go of WASD,
            // only via another explicit Attack-button press.
            r.approach_cancelled_by_input = true;
        } else if (player.derived.AttackRange > 0.f) {
            glm::vec2 to_target = {target_pos->x - player.x, target_pos->z - player.z};
            float dist_to_target = glm::length(to_target);
            if (dist_to_target > player.derived.AttackRange) {
                // Unnormalized here, same as WASD's own `dir` above — both go
                // through the identical glm::normalize() further down.
                dir = to_target;
            } else {
                // Arrived — surface it so the caller clears approach_requested.
                // Otherwise the latch would stay "active" (harmlessly inert
                // while in range, but wrong: if the target later kites back
                // out of range, approach would silently resume without a
                // fresh explicit press). The natural auto-attack resend
                // (main.cpp, every 0.85s) takes over from here.
                r.approach_arrived = true;
            }
        }
    }

    // MovementSpeedMult: character stat (from DEX, 1.0-1.3). Guard against
    // 0 (derived not yet received from server) so the player isn't frozen.
    float move_mult = player.derived.MovementSpeedMult;
    if (move_mult <= 0.f) move_mult = 1.0f;
    float base_speed = cfg_.speed * move_mult;

    // Re-evaluated AFTER the auto-approach block above may have populated
    // `dir` — this (not has_manual_input) is what gates actual movement
    // application and turn-to-face-movement below.
    bool has_move_input = glm::dot(dir, dir) > cfg_.min_dir_len_sq;

    // Turn-to-face runs whenever there's WASD input OR the player is mid-
    // attack with a valid target — the latter lets the body turn to face
    // the enemy even while standing still (dir==0), which a plain
    // "has_move_input" gate would skip entirely. Movement stays gated on
    // has_move_input ONLY, never on has_target — this is what keeps WASD
    // fully free during an attack (camera-relative strafe with the body
    // independently aimed at the target) instead of introducing any
    // attack-time movement lock, which doesn't exist anywhere else in this
    // class either. Skipped entirely during a dodge (`!dodge_active`): the
    // dash's own fixed direction (captured once at roll-start, main.cpp)
    // owns facing for its duration — letting this run here would fight
    // that fixed direction by spinning the body toward whatever WASD keys
    // happen to be held mid-roll.
    if (!dodge_active && (has_move_input || has_target)) {
        glm::vec2 move_dir(0.f);
        if (has_move_input) move_dir = glm::normalize(dir);

        // Turn-to-face: smoothly rotate player.yaw toward either (a) the
        // combat target, while mid-attack with a resolved target position
        // (overrides movement-direction facing — the whole point of this
        // feature), or (b) the world-space movement direction otherwise,
        // same as before (atan2(x, z) convention, matching click-to-move's
        // atan2(dx/dist, dz/dist)). Reuses the SAME shortest-arc-lerp
        // pattern already active for remote-actor yaw smoothing
        // (NormalizeYawDegrees/ShortestYawDeltaDegrees/SmoothLerpFactor
        // above), just with cfg_.turn_rate as the (faster, local-input)
        // rate instead of the network-catchup rate.
        float target_yaw;
        if (has_target) {
            glm::vec2 to_target = {target_pos->x - player.x, target_pos->z - player.z};
            target_yaw = glm::degrees(std::atan2f(to_target.x, to_target.y));
        } else {
            target_yaw = glm::degrees(std::atan2f(move_dir.x, move_dir.y));
        }
        float yaw_delta = ShortestYawDeltaDegrees(player.yaw, target_yaw);
        float yaw_alpha = SmoothLerpFactor(dt, cfg_.turn_rate);
        player.yaw = NormalizeYawDegrees(player.yaw + yaw_delta * yaw_alpha);
    }

    // WASD movement-magnitude calc — runs REGARDLESS of dodge (item 1: this
    // is the piece that used to be skipped entirely by the old
    // external_move_delta bypass, which is what silently dropped the
    // "additive with WASD" composition the dash relied on for its feel).
    if (has_move_input) {
        float chosen_speed =
            (moving_back && !moving_fwd) ? base_speed * cfg_.back_mult : base_speed;
        if (sprinting && !moving_back) chosen_speed *= cfg_.sprint_mult;

        glm::vec2 move_dir = glm::normalize(dir);
        any_key_move = true;
        desired_delta_2d = move_dir * chosen_speed * dt;
        if (!dodge_active) CancelMoveTarget();
    }

    if (dodge_active) {
        // Item 1: combine this frame's WASD velocity (if any — dodge is
        // always triggered while holding WASD per the dev, but this must
        // not assume that) with the dash's own fixed-direction, decaying
        // delta — additive, matching the pre-reformulation feel. Direction
        // of the dash itself is NOT recomputed here; *external_move_delta
        // already carries whatever direction+magnitude main.cpp resolved
        // once at roll-start (dodge_roll_dir) and decayed per-frame since.
        desired_delta_2d += *external_move_delta;
        auto_run_ = false;
        CancelMoveTarget();
    } else {
        // --- Click-to-move ---
        if (has_move_target_ && !any_key_move) {
            float dx = move_target_.x - player.x;
            float dz = move_target_.z - player.z;
            float d2 = dx * dx + dz * dz;
            if (d2 > cfg_.click_stop_radius * cfg_.click_stop_radius) {
                float dist = std::sqrt(d2);
                float step = std::min(base_speed * dt, dist);
                desired_delta_2d = {(dx / dist) * step, (dz / dist) * step};
                player.yaw = glm::degrees(std::atan2f(dx / dist, dz / dist));
            } else {
                CancelMoveTarget();
            }
        }

        // --- Jump (intent only — CharacterPhysics::Move() owns the actual
        // velocity/on-ground transition) ---
        jump_requested = physics_.IsOnGround() && glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (jump_requested) {
            auto_run_ = false;
        }
    }

    // --- Physics: ONE call resolves collision + gravity + ground + slope +
    // step-up together (rco::physics::CharacterPhysics::Move) ---
    physics::CapsuleMoveInput input;
    input.current_position = glm::vec3(player.x, player.y, player.z);
    input.desired_delta     = glm::vec3(desired_delta_2d.x, 0.f, desired_delta_2d.y);
    input.jump_requested     = jump_requested;
    input.dt                 = dt;
    // "Dodge não desloca" investigation (docs/TECH_DEBT.md #128) — force
    // Move()'s [slidemove]/[physics-move] logging for every frame of a
    // dodge (bounded duration), instead of the normal 1000ms throttle which
    // could otherwise skip logging the entire dash.
    input.verbose_log        = external_move_delta.has_value();

    if (external_move_delta.has_value()) {
        // Checkpoint 2 of the dodge investigation: confirms desired_delta_2d
        // (built above from external_move_delta) actually survives into
        // CapsuleMoveInput.desired_delta unchanged, right before Move() is
        // called. Cross-reference against main.cpp's [dodge]/[dodge-call]
        // (the SOURCE delta) and character_physics.cpp's [physics-move]/
        // [slidemove] (what Move() does with it).
        std::fprintf(stderr,
            "[physics-move-input] external_delta=(%.4f,%.4f) -> "
            "CapsuleMoveInput.desired_delta=(%.4f,%.4f,%.4f) player_before=(%.3f,%.3f,%.3f)\n",
            external_move_delta->x, external_move_delta->y,
            input.desired_delta.x, input.desired_delta.y, input.desired_delta.z,
            player.x, player.y, player.z);
    }

    physics::CapsuleMoveResult result = physics_.Move(input, world);

    if (external_move_delta.has_value()) {
        // Checkpoint 4: if [physics-move]/[slidemove] show a non-zero
        // posDeltaOut but player.x/z still doesn't visibly change on
        // screen, the loss is AFTER this point — either this assignment
        // isn't reached, or something later this same frame overwrites
        // player.x/y/z again (e.g. a server position-correction packet
        // applied after PlayerController::Update() returns).
        std::fprintf(stderr,
            "[physics-move-apply] result.position=(%.3f,%.3f,%.3f) "
            "player_before=(%.3f,%.3f,%.3f)\n",
            result.position.x, result.position.y, result.position.z,
            player.x, player.y, player.z);
    }

    player.x = result.position.x;
    player.y = result.position.y;
    player.z = result.position.z;

    return r;
}

} // namespace rco
