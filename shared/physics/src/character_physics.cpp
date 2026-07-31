#include "rco/physics/character_physics.h"
#include "rco/physics/capsule_sweep.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>

namespace rco::physics {

namespace {

// Diagnostic pass for the "dodge doesn't move / grudado on mesh / falls
// through Box from above" investigation (see docs/TECH_DEBT.md #125).
// ONE throttle decision per Move() call (below), threaded down into
// SlideMove/ProbeGround as a plain bool — a "logged frame"'s output is a
// small, coherent block instead of 3 independently-timed sources
// interleaving. Kept deliberately terse (a handful of lines per logged
// frame, ~once a second) after the first version of this logging turned
// out to be unreadable — dozens of lines every 200ms, one per box/triangle
// candidate tested, made testing impossible.
bool ThrottledLog(std::chrono::steady_clock::time_point& last, int ms) {
    auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::milliseconds(ms)) {
        last = now;
        return true;
    }
    return false;
}

glm::vec3 TriNormal(const renderer::ColTri& tri) {
    glm::vec3 e1 = tri.v[1] - tri.v[0];
    glm::vec3 e2 = tri.v[2] - tri.v[0];
    glm::vec3 n = glm::cross(e1, e2);
    if (glm::dot(n, glm::vec3(0.f, 1.f, 0.f)) < 0.f) n = -n; // keep "up"-facing
    float len = glm::length(n);
    return len > 1e-8f ? n / len : glm::vec3(0.f, 1.f, 0.f);
}

constexpr float kNoSurface = -3.402823466e+38F;

} // namespace

glm::vec3 CharacterPhysics::SlideMove(glm::vec3 pos, glm::vec3 delta,
                                       const CollisionWorld& world, bool doLog) const {
    const glm::vec3 deltaIn = delta;
    const glm::vec3 posIn = pos;

    for (int iter = 0; iter < cfg_.slide_iterations; ++iter) {
        if (glm::dot(delta, delta) < 1e-10f) break;

        SweepHit best;
        best.t = 1.f;
        bool anyHit = false;
        auto consider = [&](const SweepHit& h) {
            if (h.hit && h.t < best.t) { best = h; anyHit = true; }
        };
        for (const auto& box : world.col_data.boxes)  consider(SweepCapsuleVsBox(pos, cfg_.radius, cfg_.height, delta, box));
        for (const auto& box : world.dynamic_boxes)   consider(SweepCapsuleVsBox(pos, cfg_.radius, cfg_.height, delta, box));
        for (const auto& tri : world.col_data.tris)   consider(SweepCapsuleVsTriangle(pos, cfg_.radius, cfg_.height, delta, tri));
        for (const auto& sph : world.col_data.spheres) consider(SweepCapsuleVsSphere(pos, cfg_.radius, cfg_.height, delta, sph));

        if (!anyHit) {
            pos += delta;
            break;
        }

        // Step-up: if the obstacle's own top is within max_step_height of
        // our current feet, it's a climbable curb/stair, not a wall — climb
        // AND apply the full horizontal delta right here, then stop.
        //
        // BUG FIXED (confirmed by real log, see docs/TECH_DEBT.md #125):
        // this used to only raise pos.y and `continue` the loop, retrying
        // the SAME still-unconsumed delta from the new (raised) position —
        // the intent being "now that we're not blocked, the next iteration
        // will apply it normally." In practice, re-sweeping from the raised
        // position (XZ unchanged — only Y moved) frequently re-detects a
        // step (diff~0, either the same candidate now read as a floor-
        // adjacent contact, or a neighboring triangle of the same surface
        // at a near-identical height) — so the SAME branch fires again,
        // burning through all slide_iterations while XZ never advances a
        // single unit. The log showed exactly this: 3 consecutive step-up
        // hits (diff=0.079, 0.000, 0.000), then the loop just ended with
        // posDeltaOut=(0,0) — deltaIn never got a chance to apply. Climbing
        // a step is not "blocked, retry" — the step is by definition NOT a
        // wall, so treat it the same as a clean no-hit: consume the whole
        // delta immediately (real behavior change: does not re-check for a
        // second, genuinely separate obstacle further along the same path
        // at the new height in THIS SlideMove call — a real wall placed
        // right after a step would only be caught on the FOLLOWING frame's
        // call, from the now-elevated position — accepted trade-off, see
        // docs/TECH_DEBT.md #125).
        if (best.surface_top_y > kNoSurface) {
            float diff = best.surface_top_y - pos.y;
            if (diff >= -1e-4f && diff <= cfg_.max_step_height) {
                pos.y = best.surface_top_y;
                pos.x += delta.x;
                pos.z += delta.z;
                if (doLog) {
                    std::fprintf(stderr,
                        "[slidemove] iter=%d step-up diff=%.3f -> y=%.3f, ALSO applied "
                        "horizontal delta=(%.3f,%.3f) (was: continue+retry, now: apply+stop)\n",
                        iter, diff, best.surface_top_y, delta.x, delta.z);
                }
                break;
            }
        }

        if (doLog) {
            std::fprintf(stderr, "[slidemove] iter=%d hit t=%.3f normal=(%.2f,%.2f,%.2f) surfTopY=%.3f\n",
                          iter, best.t, best.normal.x, best.normal.y, best.normal.z, best.surface_top_y);
        }

        pos += delta * best.t;
        glm::vec3 remaining = delta * (1.f - best.t);
        glm::vec3 n = best.normal;
        delta = remaining - glm::dot(remaining, n) * n;
        // Tiny skin offset off the surface so float error doesn't
        // immediately re-catch the same plane next iteration.
        pos += n * 0.001f;
    }

    if (doLog) {
        glm::vec3 posDelta = pos - posIn;
        std::fprintf(stderr, "[slidemove] deltaIn=(%.3f,%.3f) posDeltaOut=(%.3f,%.3f)\n",
                      deltaIn.x, deltaIn.z, posDelta.x, posDelta.z);
    }
    return pos;
}

CharacterPhysics::GroundProbeResult CharacterPhysics::ProbeGround(
        const glm::vec3& pos, float ceiling_y, const CollisionWorld& world, bool doLog,
        bool verboseFall) const {
    GroundProbeResult best;

    // Terrain candidate — always available, ground truth. Never gated by
    // ceiling_y: unlike a box/pillar, terrain can't be "overhead."
    float terrainH = world.sample_terrain_height(pos.x, pos.z);
    glm::vec3 terrainN = world.sample_terrain_normal(pos.x, pos.z);
    best.found      = true;
    best.height     = terrainH;
    best.normal     = terrainN;
    best.is_terrain = true;
    best.slope_deg  = glm::degrees(std::acos(glm::clamp(terrainN.y, 0.f, 1.f)));

    glm::vec3 downDelta(0.f, -cfg_.ground_probe_dist, 0.f);
    auto tryBox = [&](const renderer::ColBox& box) {
        // verboseFall (item 3 "falls through Box from above" investigation):
        // logs candidates the cheap gates below are ABOUT to reject too —
        // if the real bug is the epsilon-gate excluding a box that's still
        // meaningfully above the player (see SweepCapsuleVsBox's doc
        // comment, kFloorContactEpsilon), we need to see boxes that never
        // even reach the SweepCapsuleVsBox call, not just the ones that do.
        // Restricted to boxes within a generous vertical window of pos.y so
        // this doesn't dump the whole level every frame while falling.
        const bool nearPlayer = std::abs(box.worldYMax - pos.y) <= 5.f;
        if (verboseFall && nearPlayer) {
            std::fprintf(stderr,
                "[fallprobe] box worldYMax=%.3f ceiling=%.3f pos.y=%.3f capsuleTop=%.3f "
                "(pos.y-worldYMax=%.3f) curBest=%.3f\n",
                box.worldYMax, ceiling_y, pos.y, pos.y + cfg_.height,
                pos.y - box.worldYMax, best.height);
        }
        if (box.worldYMax > ceiling_y) {
            if (verboseFall && nearPlayer) {
                std::fprintf(stderr, "[fallprobe]   -> REJECTED: worldYMax > ceiling_y (overhead gate)\n");
            }
            return;
        }
        if (box.worldYMax <= best.height) {
            if (verboseFall && nearPlayer) {
                std::fprintf(stderr, "[fallprobe]   -> REJECTED: worldYMax <= curBest (not higher)\n");
            }
            return;
        }
        SweepHit h = SweepCapsuleVsBox(pos, cfg_.radius, cfg_.height, downDelta, box);
        if (doLog || (verboseFall && nearPlayer)) {
            // Item 3 investigation — the exact epsilon-gate comparison
            // SweepCapsuleVsBox makes internally (see capsule_sweep.cpp):
            // capsuleYmin(=pos.y here, t=0 for a pure-vertical probe) vs
            // worldYMax - kFloorContactEpsilon(0.02). If pos.y is well
            // ABOVE worldYMax (still falling, not landed) and hit still
            // comes back 0 here, that confirms the gate is excluding more
            // than just "resting exactly on top."
            std::fprintf(stderr,
                "[groundprobe] box worldYMax=%.3f pos.y=%.3f (diff=%.3f) hit=%d "
                "[epsilon-gate test: pos.y(%.3f) >= worldYMax-0.02(%.3f) ? %s]\n",
                box.worldYMax, pos.y, pos.y - box.worldYMax, h.hit,
                pos.y, box.worldYMax - 0.02f,
                (pos.y >= box.worldYMax - 0.02f) ? "TRUE(excluded by epsilon gate)" : "false");
        }
        if (h.hit) {
            best.height     = box.worldYMax;
            best.normal     = glm::vec3(0.f, 1.f, 0.f); // box tops are always flat in this system
            best.is_terrain = false;
            best.slope_deg  = 0.f;
        }
    };
    for (const auto& box : world.col_data.boxes)  tryBox(box);
    for (const auto& box : world.dynamic_boxes)   tryBox(box);

    for (const auto& tri : world.col_data.tris) {
        float y;
        if (RayVerticalHitsTri(pos.x, pos.z, ceiling_y, tri, y)) {
            if (y > best.height) {
                best.height     = y;
                best.normal     = TriNormal(tri);
                best.is_terrain = false;
                best.slope_deg  = 0.f;
            }
        }
    }
    return best;
}

CapsuleMoveResult CharacterPhysics::Move(const CapsuleMoveInput& input,
                                          const CollisionWorld& world) {
    static auto s_lastLog = std::chrono::steady_clock::time_point{};
    // input.verbose_log (dodge investigation, docs/TECH_DEBT.md #128) forces
    // this call's [slidemove]/[physics-move] logging regardless of the
    // 1000ms throttle — a dodge lasts well under a second, so the throttle
    // alone could silently skip logging EVERY frame of the dash.
    const bool doLog = ThrottledLog(s_lastLog, 1000) || input.verbose_log;
    const bool on_ground_before = on_ground_;

    glm::vec3 pos = input.current_position;
    const float prevY = pos.y;

    // 3. Horizontal collide-and-slide against boxes + mesh triangles.
    //
    // NOTE (deviation from a literal "sweep terrain too" reading — flagged
    // explicitly): terrain is NOT swept here as 3D wall geometry. This
    // engine's Terrain is a 2.5D heightmap (one height per XZ column), not
    // a triangulated mesh exposed to collision — there's nothing to sweep a
    // capsule against. Terrain's role stays what it always was: a ground-
    // height/slope SOURCE, handled below via ProbeGround + the slope check
    // (still correctly never treated as a "wall" the way objects are, per
    // the existing is_terrain distinction). One real behavior change from
    // the legacy system: PlayerController::ApplyHorizontalMove used to
    // REJECT a horizontal step outright before applying it, if the
    // destination's terrain was too steep — the player was invisibly
    // walled off a cliff edge. This version always applies the horizontal
    // delta, then discovers post-hoc (via ground detection below) that
    // the destination isn't valid ground — meaning the player now WALKS OFF
    // a steep slope/cliff edge and falls, instead of being blocked there.
    // Accepted per this round's explicit risk tolerance; flag if the old
    // "can't step off a ledge" feel is actually wanted back.
    glm::vec3 horizDelta(input.desired_delta.x, 0.f, input.desired_delta.z);
    pos = SlideMove(pos, horizDelta, world, doLog);

    GroundProbeResult ground;

    if (!on_ground_) {
        // 1. Gravity.
        velocity_.y -= cfg_.gravity * input.dt;
        pos.y += velocity_.y * input.dt;

        // Item 3 investigation ("atravessar e ficar preso dentro do Box ao
        // pousar de cima") — unconditional (not throttled by doLog) frame-
        // by-frame trace of every airborne frame: bounded duration (a jump/
        // fall lasts a couple seconds at most), same reasoning as the
        // dodge-call-counter log. Compare this frame's capsuleBottom/Top
        // against the NEXT frame's to see if the capsule "jumped" from
        // above a box's worldYMax straight to inside/below it without
        // [groundprobe]/[fallprobe] ever reporting hit=1 in between.
        // 4. Ground detection — only surfaces at/below where the capsule's
        // HEAD was before this frame's fall step can catch it (same
        // invariant the legacy UpdateVertical used: never snap up through
        // something above where you started falling). BUG FIX (see
        // docs/TECH_DEBT.md #128, "atravessar e ficar preso dentro do Box
        // ao pousar de cima"): this used to be `prevY` (the capsule's FEET
        // at the start of the frame), which during a fast fall sits only a
        // hair above this frame's post-gravity `pos.y` — nowhere near the
        // capsule's actual top. Any box whose worldYMax fell between
        // prevY and prevY+height (i.e. genuinely below the player's head,
        // a legitimate landing candidate) was wrongly rejected as
        // "overhead," so the capsule fell straight through it with no hit
        // ever registering. The ceiling reference must be the capsule's
        // TOP at the start of the frame, not its feet.
        const float ceiling_y = prevY + cfg_.height;
        std::fprintf(stderr,
            "[fallprobe] FRAME prevY=%.3f ceiling_y=%.3f fellTo.y=%.3f capsuleTop=%.3f vel.y=%.3f\n",
            prevY, ceiling_y, pos.y, pos.y + cfg_.height, velocity_.y);

        ground = ProbeGround(pos, ceiling_y, world, doLog, /*verboseFall=*/true);
        std::fprintf(stderr,
            "[fallprobe] RESULT ground.found=%d ground.height=%.3f willLand=%d "
            "(pos.y=%.3f <= ground.height=%.3f)\n",
            ground.found, ground.height, ground.found && pos.y <= ground.height,
            pos.y, ground.height);
        if (ground.found && pos.y <= ground.height) {
            pos.y = ground.height;
            velocity_.y = 0.f;
            on_ground_ = true;
        }
    } else {
        // 2. Jump.
        if (input.jump_requested) {
            velocity_.y = cfg_.jump_vel;
            on_ground_ = false;
            ground = ProbeGround(pos, pos.y + cfg_.max_step_height, world, doLog);
        } else {
            // support_ceiling_y = current Y + max_step_height: the same
            // small step-up tolerance used for step-down detection below,
            // so walking past the base of a tall obstacle doesn't yank the
            // player up onto its roof.
            ground = ProbeGround(pos, pos.y + cfg_.max_step_height, world, doLog);
            float deltaH = ground.found ? (ground.height - pos.y) : -1e6f;
            if (ground.found && deltaH > -cfg_.snap_down) {
                pos.y = ground.height;

                // 6. Slope — terrain ONLY. Objects/ramps never block by
                // angle, matching the existing correct distinction (a flat
                // bridge above a steep hill must never inherit the hill's
                // slope just because it's physically above it).
                if (ground.is_terrain && ground.slope_deg > cfg_.max_slope_deg) {
                    glm::vec3 g_vec(0.f, -cfg_.gravity, 0.f);
                    glm::vec3 slide = g_vec - glm::dot(g_vec, ground.normal) * ground.normal;
                    pos.x += slide.x * input.dt;
                    pos.z += slide.z * input.dt;
                    GroundProbeResult ground2 = ProbeGround(pos, pos.y + cfg_.max_step_height, world, false);
                    if (ground2.found) { pos.y = ground2.height; ground = ground2; }
                }
            } else {
                on_ground_ = false;
                velocity_.y = 0.f;
            }
        }
    }

    // velocity_.y is already up to date from the gravity/jump/landing branches above.
    velocity_.x = input.dt > 1e-6f ? horizDelta.x / input.dt : 0.f;
    velocity_.z = input.dt > 1e-6f ? horizDelta.z / input.dt : 0.f;

    CapsuleMoveResult result;
    result.position     = pos;
    result.velocity     = velocity_;
    result.on_ground     = on_ground_;
    result.ground_normal = ground.found ? ground.normal : glm::vec3(0.f, 1.f, 0.f);

    if (doLog) {
        glm::vec3 posDelta = pos - input.current_position;
        std::fprintf(stderr,
            "[physics-move] in.delta=(%.3f,%.3f) jump=%d onGround(before=%d,after=%d) "
            "-> posDelta=(%.3f,%.3f,%.3f)\n",
            input.desired_delta.x, input.desired_delta.z, input.jump_requested,
            on_ground_before, on_ground_,
            posDelta.x, posDelta.y, posDelta.z);
    }
    return result;
}

} // namespace rco::physics
