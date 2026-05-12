#pragma once
#include <glm/glm.hpp>

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
        float turn_rate           = 150.0f;
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
        float yaw_delta     = 0.f;   // classic A/D turn: add to player.yaw AND camera
        bool  center_camera = false; // W/S without drag: camera lerps behind player
        bool  sprinting     = false;
        bool  auto_running  = false;
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

    // action_mode: when true, A/D strafe instead of turn (mouse handles rotation).
    Result Update(GLFWwindow* win, float dt, bool dead, bool action_mode,
                  PlayerState& player,
                  const renderer::Terrain& terrain,
                  bool rmb_held, bool lmb_held, bool ms_lmb_drag);

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
