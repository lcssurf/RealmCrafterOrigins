#pragma once
#include "rco/physics/collision_world.h"
#include <glm/glm.hpp>

// The unified capsule-sweep player-physics resolver — see
// docs/TECH_DEBT.md #125's architecture investigation. Replaces (for the
// LOCAL player only — see that doc for what's still shared with the legacy
// code): ColData::Resolve() (horizontal push), renderer::ComputeGroundHeight
// + renderer::SampleMeshGroundHeight (vertical ground detection),
// PlayerController::ApplyHorizontalMove's slope block, and the separate
// kMaxStepHeight bypass — all of that collapses into ONE Move() call that
// resolves position + ground state + surface normal together, the same
// shape as Unreal's CharacterMovementComponent::FindFloor/ComputeFloorDist
// and RealmCrafter-Standard's Blitz3D collision+gravity loop (both
// confirmed against real source this session).
namespace rco::physics {

struct CapsuleMoveInput {
    // Capsule FEET position — same convention as PlayerState::x/y/z today
    // (y is the bottom of the capsule, top is y+height).
    glm::vec3 current_position{0.f};
    // Desired horizontal-plane movement this frame, world-space. Y is
    // ignored on input — vertical motion is entirely gravity/ground-owned,
    // same as the legacy system (ApplyHorizontalMove never touched Y
    // either).
    glm::vec3 desired_delta{0.f};
    bool jump_requested = false;
    float dt = 0.f;
    // Forces [slidemove]/[physics-move] logging unconditionally (bypasses
    // the 1000ms doLog throttle) for this call. Set true by the caller only
    // for a naturally bounded window (e.g. PlayerController during a dodge
    // roll) — NOT meant for continuous per-frame WASD movement, which would
    // reintroduce the log spam removed earlier (see docs/TECH_DEBT.md #125,
    // "dodge não desloca" investigation).
    bool verbose_log = false;
};

struct CapsuleMoveResult {
    glm::vec3 position{0.f};
    glm::vec3 velocity{0.f};
    bool on_ground = false;
    glm::vec3 ground_normal{0.f, 1.f, 0.f};
};

class CharacterPhysics {
public:
    struct Config {
        // Matches renderer::kPlayerCapsuleRadius/Height — not read from
        // there directly (shared/physics doesn't need to know that's where
        // the "canonical" values live; the caller is responsible for
        // keeping them in sync, same as the old system's F11 debug overlay
        // already had to).
        float radius            = 0.45f;
        float height             = 1.8f;
        float gravity            = 20.0f;
        float jump_vel            = 9.0f;
        float max_slope_deg      = 45.0f;
        // Same value/reasoning as the legacy renderer::kMaxStepHeight and
        // PlayerController::Config::snap_down respectively — kept as two
        // separate knobs (step-up vs step-down tolerance) since they serve
        // different purposes even though they happen to share a value today.
        float max_step_height    = 0.5f;
        float snap_down          = 0.8f;
        // Vertical probe distance for ground detection (SweepCapsuleVsBox/
        // Triangle downward, and the ceiling gate for RayVerticalHitsTri).
        float ground_probe_dist  = 0.3f;
        // Horizontal collide-and-slide iteration count — 2-3 is the
        // standard range for resolving corners/V-shaped walls without the
        // remaining-delta ping-ponging forever.
        int   slide_iterations   = 3;
    };

    CapsuleMoveResult Move(const CapsuleMoveInput& input, const CollisionWorld& world);

    bool IsOnGround() const { return on_ground_; }
    glm::vec3 Velocity() const { return velocity_; }
    const Config& GetConfig() const { return cfg_; }
    void SetConfig(const Config& cfg) { cfg_ = cfg; }
    void Reset() { velocity_ = glm::vec3(0.f); on_ground_ = true; }

private:
    Config    cfg_{};
    glm::vec3 velocity_{0.f};
    bool      on_ground_ = true;

    // doLog: ONE throttle decision made once per Move() call and threaded
    // down here, instead of each of these having its own independent
    // timer — keeps a "logged frame"'s output as one coherent block instead
    // of 3 diagnostics interleaving on staggered schedules (see
    // docs/TECH_DEBT.md #125 — the earlier per-function-timer version was
    // unreadable, too much volume).
    glm::vec3 SlideMove(glm::vec3 pos, glm::vec3 delta, const CollisionWorld& world,
                        bool doLog) const;

    struct GroundProbeResult {
        bool      found = false;
        float     height = -3.402823466e+38F;
        glm::vec3 normal{0.f, 1.f, 0.f};
        bool      is_terrain = false;
        float     slope_deg = 0.f;
    };
    // verboseFall: unconditional (not throttled by doLog), logs EVERY box
    // candidate tested — including ones the cheap gates or the epsilon
    // exclusion reject — with the exact epsilon-gate math. Only ever passed
    // true from Move()'s airborne branch (a naturally bounded window, same
    // reasoning as the dodge call-counter log) — see docs/TECH_DEBT.md #125,
    // "atravessar e ficar preso dentro do Box ao pousar de cima"
    // investigation.
    GroundProbeResult ProbeGround(const glm::vec3& pos, float ceiling_y,
                                   const CollisionWorld& world, bool doLog,
                                   bool verboseFall = false) const;
};

} // namespace rco::physics
