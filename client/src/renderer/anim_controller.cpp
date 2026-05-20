#include "anim_controller.h"
#include <rco/renderer/actor.h>
#include <rco/renderer/pipeline.h>
#include <cstdio>

namespace rco::anim {

void AnimController::Bind(const std::vector<AnimBinding>& bindings) {
    bindings_.clear();
    action_to_id_.clear();
    active_ = {};
    active_.last_event_frame = -1;
    blend_ = {};
    current_id_ = 0;
    pending_return_ = 0xFF;

    if (bindings.empty()) return;

    // First pass: populate bindings_ and action_to_id_
    bindings_.reserve(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        bindings_.push_back(bindings[i]);
        action_to_id_[bindings[i].action] = static_cast<uint8_t>(i);
    }

    // Second pass: resolve return_to → return_to_action_id
    for (auto& b : bindings_) {
        if (b.return_to.empty()) {
            b.return_to_action_id = 0xFF;
        } else {
            auto it = action_to_id_.find(b.return_to);
            if (it != action_to_id_.end()) {
                b.return_to_action_id = it->second;
            } else {
                std::fprintf(stderr,
                    "[AnimController] Warning: return_to '%s' from action '%s' "
                    "not found in action list — ignoring\n",
                    b.return_to.c_str(), b.action.c_str());
                b.return_to_action_id = 0xFF;
            }
        }
    }

    // Diagnostic (player only — gated by log_enabled)
    if (log_enabled && !bindings_.empty()) {
        const auto& first = bindings_[0];
        std::fprintf(stderr,
            "[anim-bind] target=%p count=%zu first='%s' speed=%.3f blend=%.3f\n",
            (void*)this, bindings_.size(),
            first.action.c_str(), first.speed, first.blend_in);
    }

    // Directly initialize active_ to bindings_[0]. Without this, RequestState(0) always
    // early-returns (new_id==current_id_==0 && !active_.finished), leaving binding=nullptr.
    active_.binding          = &bindings_[0];
    active_.time_sec         = 0.f;
    active_.last_event_frame = bindings_[0].start_frame - 1;
    active_.finished         = false;
    pending_return_          = bindings_[0].return_to_action_id;
    current_id_              = 0;

    // If Idle is present, transition into it (updates current_id_ and blend state).
    RequestStateByName("Idle");
}

// Public entry point — logs [request-external] so callers (server packets, input)
// are visible in the trace. Delegates to the internal impl with force=false.
bool AnimController::RequestState(uint8_t new_id) {
    if (log_enabled && !bindings_.empty() && new_id < static_cast<uint8_t>(bindings_.size()))
        std::fprintf(stderr, "[request-external] action_id=%u action='%s'\n",
            (unsigned)new_id, bindings_[new_id].action.c_str());
    return RequestStateImpl_(new_id, /*force=*/false);
}

bool AnimController::ForceState(uint8_t new_id) {
    return RequestStateImpl_(new_id, /*force=*/true);
}

bool AnimController::RequestStateImpl_(uint8_t new_id, bool force) {
    if (bindings_.empty() || new_id >= static_cast<uint8_t>(bindings_.size())) {
        if (log_enabled)
            std::fprintf(stderr, "[anim-request-result] new_id=%u cur_id=%u result=invalid_id\n",
                (unsigned)new_id, (unsigned)current_id_);
        return false;
    }

    // No-op if already in this state and not finished
    if (!force && new_id == current_id_ && !active_.finished) {
        return true;
    }

    const auto& nb = bindings_[new_id];
    const auto& cb = bindings_[current_id_];

    // Priority check: only block if current is a one-shot still playing.
    // force=true bypasses this so a naturally-ended one-shot can always return
    // to its return_to action regardless of priority.
    bool current_is_oneshot = !cb.loop;
    if (!force && current_is_oneshot && !active_.finished && nb.priority < cb.priority) {
        if (log_enabled)
            std::fprintf(stderr,
                "[anim-request-result] new_id=%u cur_id=%u result=priority_block"
                " new_prio=%u cur_prio=%u\n",
                (unsigned)new_id, (unsigned)current_id_,
                (unsigned)nb.priority, (unsigned)cb.priority);
        return false;
    }

    // Set up blend
    blend_.from_action = cb.action;
    blend_.from_time   = active_.time_sec;
    blend_.elapsed     = 0.f;
    blend_.duration    = nb.blend_in;
    blend_.active      = (nb.blend_in > 0.001f);

    // Switch to new binding
    active_.binding          = &bindings_[new_id];
    active_.time_sec         = 0.f;
    active_.last_event_frame = nb.start_frame - 1;
    active_.finished         = false;

    pending_return_ = nb.return_to_action_id;
    current_id_     = new_id;
    if (log_enabled) {
        std::fprintf(stderr, "[anim-request-result] new_id=%u cur_id=%u result=ok\n",
            (unsigned)new_id, (unsigned)current_id_);
        std::fprintf(stderr,
            "[anim-request-ok] action='%s' speed=%.3f clip='%s'\n",
            nb.action.c_str(), nb.speed, nb.source_path.c_str());
    }
    return true;
}

bool AnimController::RequestStateByName(const std::string& action) {
    auto it = action_to_id_.find(action);
    if (it == action_to_id_.end()) return false;
    if (bindings_[it->second].source_path.empty()) {
        if (log_enabled)
            std::fprintf(stderr,
                "[anim-request-result] action='%s' id=%u result=no_clip\n",
                action.c_str(), (unsigned)it->second);
        return false;
    }
    return RequestStateImpl_(it->second, /*force=*/false);
}

void AnimController::Update(float dt, float speed) {
    if (bindings_.empty() || !active_.binding) return;

    // Log once whenever the active action changes (player only — gated by log_enabled).
    if (log_enabled) {
        static uint8_t s_last_id = 0xFF;
        if (current_id_ != s_last_id) {
            s_last_id = current_id_;
            float active_speed = active_.binding ? active_.binding->speed : 0.f;
            std::fprintf(stderr,
                "[anim-update] target=%p action='%s' binding_speed=%.3f cur_id=%u\n",
                (void*)this,
                bindings_[current_id_].action.c_str(),
                active_speed, (unsigned)current_id_);
        }
    }

    // 1. Auto-locomotion — pick the best available locomotion state for current speed
    {
        const std::string& cur = bindings_[current_id_].action;
        if (cur == "Idle" || cur == "Walk" || cur == "Run") {
            const char* desired;
            if      (speed > 3.0f  && HasAction("Run"))  desired = "Run";
            else if (speed > 0.15f && HasAction("Walk")) desired = "Walk";
            else                                          desired = "Idle";

            if (cur != desired) {
                if (log_enabled) {
                    static const char* s_last_target = nullptr;
                    if (desired != s_last_target) {
                        s_last_target = desired;
                        std::fprintf(stderr, "[auto-loco] cur='%s' speed=%.3f -> '%s'\n",
                            cur.c_str(), speed, desired);
                    }
                }
                RequestStateByName(desired);
            }
        }
    }

    // 2. Advance time
    const auto& b = *active_.binding;
    active_.time_sec += dt * (b.speed > 0.f ? b.speed : 1.f);
    float effective_fps = b.fps > 0.f ? b.fps : 30.f;
    int32_t cur_frame = static_cast<int32_t>(active_.time_sec * effective_fps) + b.start_frame;

    // 3. Dispatch events between last_event_frame and cur_frame
    DispatchEvents(active_.last_event_frame, cur_frame);
    active_.last_event_frame = cur_frame;

    // 4. End-of-clip detection
    // Frame-based when end_frame is known; time-based fallback when end_frame == -1
    // and SetClipDuration() has been called to supply the real duration.
    int32_t end_frame   = b.end_frame;
    bool    frame_ended = (end_frame > 0 && cur_frame >= end_frame);
    bool    time_ended  = (!frame_ended && end_frame == -1 &&
                           b.duration_sec > 0.f && active_.time_sec >= b.duration_sec);
    if (frame_ended || time_ended) {
        if (b.loop) {
            active_.time_sec         = 0.f;
            active_.last_event_frame = b.start_frame - 1;
        } else {
            if (log_enabled)
                std::fprintf(stderr, "[anim-finished] action='%s' -> '%s'\n",
                    b.action.c_str(),
                    pending_return_ != 0xFF ? bindings_[pending_return_].action.c_str() : "(none)");
            if (pending_return_ != 0xFF) {
                uint8_t ret     = pending_return_;
                pending_return_ = 0xFF;
                RequestStateImpl_(ret, /*force=*/true);
            } else {
                active_.finished = true;
            }
        }
    }

    // 5. Advance blend
    if (blend_.active) {
        blend_.elapsed   += dt;
        blend_.from_time += dt * b.speed;  // keep from-anim advancing too
        if (blend_.elapsed >= blend_.duration) {
            blend_.active = false;
        }
    }
}

void AnimController::DispatchEvents(int32_t from_f, int32_t to_f) {
    if (!active_.binding) return;
    for (const auto& ev : active_.binding->events) {
        if (ev.frame > from_f && ev.frame <= to_f) {
            auto it = handlers_.find(ev.event_type);
            if (it != handlers_.end()) {
                for (auto& h : it->second) h(ev);
            }
        }
    }
}

void AnimController::SetClipDuration(const std::string& action, float dur_sec) {
    auto it = action_to_id_.find(action);
    if (it != action_to_id_.end())
        bindings_[it->second].duration_sec = dur_sec;
}

void AnimController::OnEvent(const std::string& event_type, EventHandler h) {
    handlers_[event_type].push_back(std::move(h));
}

void AnimController::Submit(rco::renderer::Actor& actor,
                            rco::renderer::Pipeline& pipeline) const {
    if (!IsReady()) {
        actor.Submit(pipeline);
        return;
    }
    if (IsBlending()) {
        actor.SubmitBlended(pipeline,
            BlendFromAction(), BlendFromTime(),
            CurrentAction(),   CurrentTime(),
            BlendAlpha());
    } else {
        bool loop_flag = active_.binding ? active_.binding->loop : true;
        actor.SubmitAs(CurrentAction(), CurrentTime(), loop_flag, pipeline);
    }
}

} // namespace rco::anim
