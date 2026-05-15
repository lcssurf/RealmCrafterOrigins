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
        bool rmb_held, bool lmb_held)
{
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

    float yr = glm::radians(player.yaw);
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

    if (glm::dot(dir, dir) > cfg_.min_dir_len_sq) {
        dir = glm::normalize(dir);
        float chosen_speed =
            (moving_back && !moving_fwd) ? cfg_.speed * cfg_.back_mult : cfg_.speed;
        if (sprinting && !moving_back) chosen_speed *= cfg_.sprint_mult;

        CancelMoveTarget();
        any_key_move = true;
        ApplyHorizontalMove(dir * chosen_speed * dt, player, terrain);
    }

    // --- Click-to-move ---
    if (has_move_target_ && !any_key_move) {
        float dx = move_target_.x - player.x;
        float dz = move_target_.z - player.z;
        float d2 = dx * dx + dz * dz;
        if (d2 > cfg_.click_stop_radius * cfg_.click_stop_radius) {
            float dist = std::sqrt(d2);
            float step = std::min(cfg_.speed * dt, dist);
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

