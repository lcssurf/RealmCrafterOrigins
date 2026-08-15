#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace rco::renderer { class Actor; class Pipeline; }

namespace rco::anim {

// ── Structs ──────────────────────────────────────────────────────────────────

struct AnimEvent {
    int32_t     frame;
    std::string event_type;
    std::string payload;
};

struct AnimBinding {
    std::string action;
    std::string source_path;
    int32_t     start_frame = 0;
    int32_t     end_frame   = -1;  // -1 = play to file end
    float       fps         = 30.f;
    bool        loop        = true;
    float       speed       = 1.f;
    float       blend_in    = 0.15f;
    std::string return_to;
    uint8_t     priority    = 0;
    // Explicit "terminal state" signal (docs/TECH_DEBT.md, mob-death-stuck-
    // in-Idle investigation) — distinguishes a one-shot with no return_to
    // BY DESIGN (e.g. Death: should hold its last frame forever) from one
    // missing return_to BY MISTAKE (the case Update()'s end-of-clip
    // fallback-to-Idle exists to rescue). Default false preserves existing
    // behavior for every binding that doesn't set it. NOT inferred from
    // priority — a coincidentally-high-priority binding must not be treated
    // as terminal.
    bool        is_terminal  = false;
    float       duration_sec = 0.f;  // set via SetClipDuration() after clip load; 0 = unknown
    std::vector<AnimEvent> events;
    uint8_t     return_to_action_id = 0xFF;  // resolved in Bind()
};

struct ActiveAnim {
    const AnimBinding* binding  = nullptr;
    float  time_sec             = 0.f;
    int32_t last_event_frame    = -1;
    bool   finished             = false;
};

struct BlendState {
    std::string from_action;   // action name being blended FROM
    float       from_time             = 0.f;
    float       from_start_offset_sec = 0.f;  // start_frame/fps of the from-binding
    float       elapsed               = 0.f;
    float       duration              = 0.f;
    bool        active                = false;
};

// Returns true when a binding has usable animation data.
// Embedded clips (source_path empty) are always ready — data lives in the model file.
// External file clips become ready once LoadAnim runs and SetClipDuration is called.
inline bool AnimBindingIsValid(const AnimBinding& b) {
    return b.source_path.empty() || b.duration_sec > 0.f;
}

// ── AnimController ────────────────────────────────────────────────────────────

// Per-actor state machine. Manages which binding is active, blending,
// auto-locomotion (Idle/Walk/Run by velocity), and animation event dispatch.
//
// Usage:
//   1. Call Bind(bindings) once on PNewActor.
//   2. Call RequestState(action_id) when PAnimateActor arrives.
//   3. Call Update(dt, velocity) every frame.
//   4. Read CurrentAction() / CurrentTime() to drive the renderer.
class AnimController {
public:
    using EventHandler = std::function<void(const AnimEvent&)>;

    // Bind the full action table received in PNewActor. Must be called before
    // any other method. Resolves return_to → return_to_action_id.
    void Bind(const std::vector<AnimBinding>& bindings);

    // Request a state transition by action_id (0-based index from PNewActor).
    // Returns false if id is out of range or lower-priority than current.
    // origin: short caller tag (e.g. "PAnimateActor/other-actor"), used ONLY
    // by the unconditional [death-term][TO_IDLE] trace in RequestStateImpl_
    // (docs/TECH_DEBT.md #129 round 3 — "something forces Idle after the
    // terminal branch already ran correctly"). Every call site in the
    // codebase should pass a distinct tag so a real log pinpoints exactly
    // which one fired.
    bool RequestState(uint8_t action_id, const char* origin = "?");

    // Force a state transition by action_id, bypassing priority-block checks.
    bool ForceState(uint8_t action_id, const char* origin = "?");

    // Convenience: request by name.
    bool RequestStateByName(const std::string& action, const char* origin = "?");

    // Convenience: force by name, bypassing priority-block checks — same
    // relationship to ForceState() that RequestStateByName() has to
    // RequestState(). Prefer this over ForceState(hardcoded_index) at any
    // call site: bindings_ order is whatever the DB query happened to
    // return (see docs/TECH_DEBT.md, respawn-stuck-in-Walk/Run
    // investigation — media_actor_anims was loaded with no ORDER BY, so
    // "index 0 == Idle" was never a real guarantee).
    bool ForceStateByName(const std::string& action, const char* origin = "?");

    // Advance the animation state by dt seconds.
    // speed is the actor's movement speed (for auto-locomotion).
    void Update(float dt, float speed);

    // Raw AttackSpeedMult (the same derived stat that already drives the
    // real attack-cooldown timing server-side, combat_basic.go:46-50) — NOT
    // used as a playback multiplier directly. Update() derives the actual
    // playback-rate from it: natural_clip_duration / real_attack_window,
    // where real_attack_window = CombatDelay/AttackSpeedMult, so the Attack
    // clip always plays start-to-finish exactly once per real attack
    // window regardless of the clip's own authored length. Only applies
    // while the active binding's action is "Attack" or a weapon-style
    // composite of it ("Attack_X" — see world.Actor.BasicAttackAnimStyle
    // server-side); locomotion (Idle/Walk/Run) and every other action
    // ignore this entirely, so a fast attacker doesn't also sprint/idle in
    // fast-forward. <=0 is treated as "no scaling" (Update() skips the
    // whole computation when this is <=0).
    void SetAttackSpeedMult(float m) { attack_speed_mult_ = m > 0.f ? m : 1.f; }

    // Equipped weapon's grip/pose archetype (item_templates.weapon_anim_style
    // — "sword_1h", "bow"...), same value world.Actor.BasicAttackAnimStyle
    // carries server-side. Used ONLY by Update()'s auto-locomotion block to
    // compose "Walk"/"Run"/"Idle" into "Walk_bow" etc, mirroring
    // world.ComposeWeaponAction (server, Go) for the ONE path that never
    // goes through the server at all: local auto-locomotion is decided by
    // this client purely from measured velocity (see Update()'s doc
    // comment) — RequestState never fires for it, so BroadcastAnimate/
    // ComposeWeaponAction server-side can't reach it no matter what. "" =
    // no style configured, falls back to the plain base action exactly like
    // before this existed.
    void SetWeaponAnimStyle(const std::string& style) { weapon_anim_style_ = style; }

    // General case of the same "scale playback to fit a known real-world
    // window" principle SetAttackSpeedMult applies to the Attack family —
    // for anything else with a per-cast target duration, e.g. an ability's
    // windup: server-side ability_templates.windup_ms (combat_special.go's
    // startNPCSpecialCast/SpawnScriptedImpact), NOT CombatDelay/
    // AttackSpeedMult like the basic attack. The server has no idea which
    // clip a given Actor Def bound to that action or how long it naturally
    // runs — only the CLIENT can compute natural_duration/target and scale
    // accordingly, so this has to be set by whoever correlates the combat
    // event carrying the windup duration with the action name about to
    // play (main.cpp, see pending_windup_target_by_rid).
    //
    // target_duration_sec <= 0 clears any override for that action name
    // (falls back to natural, unscaled playback). Entries are NOT
    // auto-cleared when the action finishes — callers should overwrite or
    // clear explicitly per cast; a stale entry only matters if the SAME
    // action name is requested again with a genuinely different intended
    // duration, which main.cpp's per-cast correlation already handles by
    // setting a fresh value before every RequestState for a windup action.
    void SetActionTargetDuration(const std::string& action, float target_duration_sec) {
        if (target_duration_sec > 0.f) action_target_duration_sec_[action] = target_duration_sec;
        else action_target_duration_sec_.erase(action);
    }

    // True if Bind() has been called with at least one binding.
    bool IsReady() const { return !bindings_.empty(); }

    bool HasAction(const std::string& action) const {
        auto it = action_to_id_.find(action);
        if (it == action_to_id_.end()) return false;
        return AnimBindingIsValid(bindings_[it->second]);
    }
    const std::string& CurrentAction() const {
        static const std::string empty;
        if (bindings_.empty()) return empty;
        return bindings_[current_id_].action;
    }
    // Action name bound to a given id, WITHOUT requesting it — lets a
    // caller correlate an about-to-be-requested action_id (e.g. from a
    // PAnimateActor packet) with external data (SetActionTargetDuration)
    // before actually calling RequestState. "" if id is out of range.
    const std::string& ActionNameById(uint8_t id) const {
        static const std::string empty;
        if (id >= static_cast<uint8_t>(bindings_.size())) return empty;
        return bindings_[id].action;
    }
    uint8_t CurrentActionId() const { return current_id_; }
    float   CurrentTime()    const { return active_.time_sec; }

    // Blend state — for callers that want to implement blended rendering.
    bool        IsBlending()       const { return blend_.active; }
    const std::string& BlendFromAction() const { return blend_.from_action; }
    float       BlendFromTime()    const { return blend_.from_time; }
    float       BlendAlpha()       const {
        if (!blend_.active || blend_.duration <= 0.f) return 1.f;
        float t = blend_.elapsed / blend_.duration;
        return t * t * (3.f - 2.f * t);  // smoothstep
    }

    // Set the real clip duration (seconds) for a binding after it's been loaded
    // by the renderer. Used for time-based end-of-clip detection when end_frame == -1.
    void SetClipDuration(const std::string& action, float dur_sec);

    // Register a handler for an event type (e.g. "hitbox", "footstep", "sfx").
    void OnEvent(const std::string& event_type, EventHandler h);

    // Drive actor rendering for this frame: delegates to SubmitBlended when a
    // crossfade is in progress, SubmitAs otherwise. Falls back to actor.Submit()
    // when no bindings have been registered yet.
    void Submit(rco::renderer::Actor& actor, rco::renderer::Pipeline& pipeline) const;

    // Set to true on a specific controller instance to enable verbose stderr
    // diagnostics. Off by default so NPC controllers stay silent.
    bool log_enabled = false;

private:
    // Core transition logic. force=true bypasses the priority check (used when
    // a one-shot finishes naturally and must return to its return_to action).
    bool RequestStateImpl_(uint8_t action_id, bool force, const char* origin = "?");
    void DispatchEvents(int32_t from_frame, int32_t to_frame);

    std::vector<AnimBinding>                    bindings_;
    std::unordered_map<std::string, uint8_t>    action_to_id_;
    std::unordered_map<std::string, std::vector<EventHandler>> handlers_;

    uint8_t     current_id_      = 0;
    uint8_t     pending_return_  = 0xFF;
    ActiveAnim  active_;
    BlendState  blend_;
    float       attack_speed_mult_ = 1.f;
    std::string weapon_anim_style_;
    std::unordered_map<std::string, float> action_target_duration_sec_;
};

} // namespace rco::anim
