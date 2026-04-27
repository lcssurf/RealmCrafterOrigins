#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

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
    float       from_time = 0.f;
    float       elapsed   = 0.f;
    float       duration  = 0.f;
    bool        active    = false;
};

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
    bool RequestState(uint8_t action_id);

    // Convenience: request by name.
    bool RequestStateByName(const std::string& action);

    // Advance the animation state by dt seconds.
    // speed is the actor's movement speed (for auto-locomotion).
    void Update(float dt, float speed);

    // True if Bind() has been called with at least one binding.
    bool IsReady() const { return !bindings_.empty(); }

    bool HasAction(const std::string& action) const {
        auto it = action_to_id_.find(action);
        if (it == action_to_id_.end()) return false;
        return !bindings_[it->second].source_path.empty();
    }
    const std::string& CurrentAction() const {
        static const std::string empty;
        if (bindings_.empty()) return empty;
        return bindings_[current_id_].action;
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

    // Set to true on a specific controller instance to enable verbose stderr
    // diagnostics. Off by default so NPC controllers stay silent.
    bool log_enabled = false;

private:
    // Core transition logic. force=true bypasses the priority check (used when
    // a one-shot finishes naturally and must return to its return_to action).
    bool RequestStateImpl_(uint8_t action_id, bool force);
    void DispatchEvents(int32_t from_frame, int32_t to_frame);

    std::vector<AnimBinding>                    bindings_;
    std::unordered_map<std::string, uint8_t>    action_to_id_;
    std::unordered_map<std::string, std::vector<EventHandler>> handlers_;

    uint8_t     current_id_      = 0;
    uint8_t     pending_return_  = 0xFF;
    ActiveAnim  active_;
    BlendState  blend_;
};

} // namespace rco::anim
