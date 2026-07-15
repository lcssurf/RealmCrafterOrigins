#include "player_controller.h"
#include "../ui/game_state.h"
#include "../renderer/terrain/terrain.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace rco {
namespace {
constexpr bool kDebugPlayerMovement = false;

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
// ApplyHorizontalMove
// Only blocks uphill movement that exceeds max_slope_deg (measured from
// current height to destination height). Flat or downhill always passes.
// Blocked movement is projected sideways along the slope face.
// ---------------------------------------------------------------------------
bool PlayerController::ApplyHorizontalMove(glm::vec2 delta,
        PlayerState& player,
        const renderer::Terrain& terrain)
{
    if (glm::dot(delta, delta) < cfg_.min_move_len_sq) return true;

    float new_x = player.x + delta.x;
    float new_z = player.z + delta.y;

    float h_src = terrain.SampleHeight(player.x, player.z);
    float h_dst = terrain.SampleHeight(new_x, new_z);
    float rise  = h_dst - h_src;
    float dist  = glm::length(delta);

    if (rise > 0.f) {
        float slope_deg = glm::degrees(std::atan2f(rise, dist));
        if (slope_deg > cfg_.max_slope_deg) {
            // Project sideways along slope face
            glm::vec3 n  = terrain.SampleNormal(new_x, new_z);
            glm::vec3 d3 = glm::normalize(glm::vec3(delta.x, 0.f, delta.y));
            glm::vec3 proj = d3 - glm::dot(d3, n) * n;

            glm::vec2 proj2(proj.x, proj.z);
            float plen = glm::length(proj2);
            if (plen > cfg_.min_proj_len) {
                new_x = player.x + (proj2.x / plen) * dist;
                new_z = player.z + (proj2.y / plen) * dist;

                float h_proj = terrain.SampleHeight(new_x, new_z);
                float rise2  = h_proj - h_src;
                float dist2  = glm::length(glm::vec2(new_x - player.x, new_z - player.z));
                if (rise2 > 0.f && dist2 > cfg_.min_proj_len &&
                    glm::degrees(std::atan2f(rise2, dist2)) > cfg_.max_slope_deg) {
                    if (kDebugPlayerMovement) {
                        std::fprintf(stderr,
                            "[move] blocked uphill slope=%.1f max=%.1f pos=(%.2f,%.2f)\n",
                            glm::degrees(std::atan2f(rise2, dist2)),
                            cfg_.max_slope_deg, player.x, player.z);
                    }
                    return false;
                }
            } else {
                if (kDebugPlayerMovement) {
                    std::fprintf(stderr,
                        "[move] blocked no-side-proj pos=(%.2f,%.2f)\n",
                        player.x, player.z);
                }
                return false;
            }
        }
    }

    player.x = new_x;
    player.z = new_z;
    return true;
}

// ---------------------------------------------------------------------------
// UpdateVertical
// Snaps to terrain when grounded. Goes airborne when terrain drops more than
// snap_down in one frame. Applies gravity when airborne. Slides on steep slopes.
// ---------------------------------------------------------------------------
void PlayerController::UpdateVertical(float dt,
        PlayerState& player,
        const renderer::Terrain& terrain)
{
    float terrain_h = terrain.SampleHeight(player.x, player.z);

    if (!on_ground_) {
        vel_y_   -= cfg_.gravity * dt;
        player.y += vel_y_ * dt;
        if (player.y <= terrain_h) {
            player.y   = terrain_h;
            vel_y_     = 0.f;
            on_ground_ = true;
            if (kDebugPlayerMovement) {
                std::fprintf(stderr, "[move] landed y=%.2f\n", player.y);
            }
        }
        return;
    }

    float delta_h = terrain_h - player.y;

    if (delta_h > -cfg_.snap_down) {
        player.y = terrain_h;
    } else {
        on_ground_ = false;
        vel_y_     = 0.f;
        if (kDebugPlayerMovement) {
            std::fprintf(stderr, "[move] leave-ground drop=%.2f\n", delta_h);
        }
        return;
    }

    // Slope slide
    glm::vec3 n     = terrain.SampleNormal(player.x, player.z);
    float slope_deg = glm::degrees(std::acos(glm::clamp(n.y, 0.f, 1.f)));
    if (slope_deg > cfg_.max_slope_deg) {
        glm::vec3 g_vec(0.f, -cfg_.gravity, 0.f);
        glm::vec3 slide = g_vec - glm::dot(g_vec, n) * n;
        player.x += slide.x * dt;
        player.z += slide.z * dt;
        player.y  = terrain.SampleHeight(player.x, player.z);
        if (kDebugPlayerMovement) {
            std::fprintf(stderr,
                "[move] slope-slide angle=%.1f max=%.1f pos=(%.2f,%.2f)\n",
                slope_deg, cfg_.max_slope_deg, player.x, player.z);
        }
    }
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------
PlayerController::Result PlayerController::Update(
        GLFWwindow* win, float dt, bool dead,
        PlayerState& player,
        const renderer::Terrain& terrain,
        float camera_yaw,
        bool rmb_held, bool lmb_held,
        bool is_attacking,
        std::optional<glm::vec3> target_pos,
        bool approach_requested)
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
        player.y   = terrain.SampleHeight(player.x, player.z);
        vel_y_     = 0.f;
        on_ground_ = true;
        auto_run_  = false;
        CancelMoveTarget();
        return r;
    }

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
    // button press, not passively from having a target.
    bool has_manual_input = glm::dot(dir, dir) > cfg_.min_dir_len_sq;
    if (approach_requested && target_pos.has_value()) {
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
    // "has_move_input" gate would skip entirely. Movement (ApplyHorizontalMove
    // below) stays gated on has_move_input ONLY, never on has_target — this
    // is what keeps WASD fully free during an attack (camera-relative
    // strafe with the body independently aimed at the target) instead of
    // introducing any attack-time movement lock, which doesn't exist
    // anywhere else in this class either.
    if (has_move_input || has_target) {
        glm::vec2 move_dir(0.f);
        if (has_move_input) move_dir = glm::normalize(dir);

        // Turn-to-face: smoothly rotate player.yaw toward either (a) the
        // combat target, while mid-attack with a resolved target position
        // (overrides movement-direction facing — the whole point of this
        // feature), or (b) the world-space movement direction otherwise,
        // same as before (atan2(x, z) convention, matching
        // ApplyHorizontalMove/click-to-move's atan2(dx/dist, dz/dist)).
        // Reuses the SAME shortest-arc-lerp pattern already active for
        // remote-actor yaw smoothing (NormalizeYawDegrees/
        // ShortestYawDeltaDegrees/SmoothLerpFactor above), just with
        // cfg_.turn_rate as the (faster, local-input) rate instead of the
        // network-catchup rate — same rate/feel whether facing a target or
        // facing movement, so there's no jarring speed change when the
        // attack window starts/ends.
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

        if (has_move_input) {
            float chosen_speed =
                (moving_back && !moving_fwd) ? base_speed * cfg_.back_mult : base_speed;
            if (sprinting && !moving_back) chosen_speed *= cfg_.sprint_mult;

            CancelMoveTarget();
            any_key_move = true;
            ApplyHorizontalMove(move_dir * chosen_speed * dt, player, terrain);
        }
    }

    // --- Click-to-move ---
    if (has_move_target_ && !any_key_move) {
        float dx = move_target_.x - player.x;
        float dz = move_target_.z - player.z;
        float d2 = dx * dx + dz * dz;
        if (d2 > cfg_.click_stop_radius * cfg_.click_stop_radius) {
            float dist = std::sqrt(d2);
            float step = std::min(base_speed * dt, dist);
            ApplyHorizontalMove({(dx / dist) * step, (dz / dist) * step}, player, terrain);
            player.yaw = glm::degrees(std::atan2f(dx / dist, dz / dist));
        } else {
            CancelMoveTarget();
        }
    }

    // --- Jump ---
    if (on_ground_ && glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS) {
        vel_y_ = cfg_.jump_vel;
        on_ground_ = false;
        auto_run_ = false;
        if (kDebugPlayerMovement) {
            std::fprintf(stderr, "[move] jump vel=%.2f\n", cfg_.jump_vel);
        }
    }

    // --- Vertical ---
    UpdateVertical(dt, player, terrain);

    return r;
}

} // namespace rco

