#include "anim_controller.h"
#include <rco/renderer/actor.h>
#include <rco/renderer/pipeline.h>
#include <cstdio>

namespace rco::anim {

namespace {
// FIX 1: sane placeholder duration (seconds) for a non-loop binding whose
// end_frame is invalid/unset (<= start_frame, includes the -1 sentinel).
// See the long comment in Update() for why this exists.
constexpr float kDefaultOneShotFallbackDurationSec = 1.0f;
} // namespace

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
        // UNCONDITIONAL (see docs/TECH_DEBT.md #129 round 2): traces
        // is_terminal from the moment it arrives in this AnimController,
        // BEFORE any Update()/end-of-clip logic runs. If this prints
        // is_terminal=0 for Death, the field never arrived true here —
        // the bug is upstream (DB read, wire encode/decode, or a struct
        // copy site in main.cpp that wasn't updated), not in this file.
        if (b.action == "Death" || b.is_terminal) {
            std::fprintf(stderr,
                "[death-term][BIND] action='%s' is_terminal=%d return_to='%s' priority=%u\n",
                b.action.c_str(), (int)b.is_terminal, b.return_to.c_str(),
                static_cast<unsigned>(b.priority));
        }
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

    // Directly initialize active_ to bindings_[0]. Without this, RequestState(0) always
    // early-returns (new_id==current_id_==0 && !active_.finished), leaving binding=nullptr.
    active_.binding          = &bindings_[0];
    active_.time_sec         = 0.f;
    active_.last_event_frame = bindings_[0].start_frame - 1;
    active_.finished         = false;
    pending_return_          = bindings_[0].return_to_action_id;
    current_id_              = 0;

    // If Idle is present, transition into it (updates current_id_ and blend state).
    RequestStateByName("Idle", "AnimController::Bind");
}

bool AnimController::RequestState(uint8_t new_id, const char* origin) {
    return RequestStateImpl_(new_id, /*force=*/false, origin);
}

bool AnimController::ForceState(uint8_t new_id, const char* origin) {
    return RequestStateImpl_(new_id, /*force=*/true, origin);
}

bool AnimController::RequestStateImpl_(uint8_t new_id, bool force, const char* origin) {
    if (bindings_.empty() || new_id >= static_cast<uint8_t>(bindings_.size())) {
        if (log_enabled) {
            std::fprintf(stderr,
                "[anim-diag][REQUEST] REJECTED id=%u out of range (bindings_=%zu)\n",
                static_cast<unsigned>(new_id), bindings_.size());
        }
        return false;
    }

    // No-op if already in this state and not finished
    if (!force && new_id == current_id_ && !active_.finished) {
        if (log_enabled) {
            std::fprintf(stderr,
                "[anim-diag][REQUEST] no-op: already in '%s' (id=%u) and not finished\n",
                bindings_[new_id].action.c_str(), static_cast<unsigned>(new_id));
        }
        return true;
    }

    const auto& nb = bindings_[new_id];
    const auto& cb = bindings_[current_id_];

    // UNCONDITIONAL (see docs/TECH_DEBT.md #129 round 3): every single call
    // into this function that targets Idle, no matter the caller, no matter
    // whether the current state is terminal/finished. Round 2's
    // [death-term][OVERRIDE] below only fired when leaving a
    // terminal+finished state — round 3's real log showed the terminal
    // branch (is_terminal=true, current_id_ stays at Death, active_.finished
    // =true) firing correctly EVERY frame, yet the actor still visually
    // ends up on Idle. This trace is broader on purpose: it fires for EVERY
    // transition targeting Idle regardless of what cb/current state looks
    // like, so it also catches paths that don't go through this exact
    // instance's "leaving a terminal state" framing (e.g. a fresh Bind()
    // that resets current_id_ to 0 BEFORE calling RequestStateByName("Idle")
    // — cb at that point would already be bindings_[0], not Death, so the
    // OVERRIDE check below would never fire for that path, but this one will).
    if (nb.action == "Idle") {
        std::fprintf(stderr,
            "[death-term][TO_IDLE] origin='%s' force=%d requested_id=%u ('Idle') <- "
            "current_id_=%u ('%s') active_.finished=%d cb.is_terminal=%d\n",
            origin, (int)force, static_cast<unsigned>(new_id),
            static_cast<unsigned>(current_id_), cb.action.c_str(),
            (int)active_.finished, (int)cb.is_terminal);
    }

    // UNCONDITIONAL (see docs/TECH_DEBT.md #129 round 2, item 3): catches
    // ANY caller that requests a state change AWAY from a terminal,
    // already-finished state (e.g. Death after is_terminal correctly held
    // it) — this is exactly the "something else forces Idle afterward"
    // scenario the previous round's fix would NOT cover, since that fix
    // only touches the end-of-clip branch, not every possible caller of
    // RequestState/RequestStateByName/ForceState/Bind() elsewhere in the
    // codebase (loco system, corpse re-bind, a stray PNewActor, etc).
    if (cb.is_terminal && active_.finished && new_id != current_id_) {
        std::fprintf(stderr,
            "[death-term][OVERRIDE] origin='%s' '%s' (terminal, finished=true) is being "
            "switched to '%s' (id=%u) by an external RequestState/ForceState call, "
            "force=%d.\n",
            origin, cb.action.c_str(), nb.action.c_str(), static_cast<unsigned>(new_id),
            (int)force);
    }

    if (log_enabled) {
        std::fprintf(stderr,
            "[anim-diag][REQUEST] action='%s' id=%u start=%d end=%d loop=%d "
            "return_to='%s'(id=%u) priority=%u force=%d  <-- from cur='%s'(id=%u) "
            "cur_loop=%d cur_priority=%u cur_finished=%d\n",
            nb.action.c_str(), static_cast<unsigned>(new_id),
            nb.start_frame, nb.end_frame, (int)nb.loop,
            nb.return_to.c_str(), static_cast<unsigned>(nb.return_to_action_id),
            static_cast<unsigned>(nb.priority), (int)force,
            cb.action.c_str(), static_cast<unsigned>(current_id_),
            (int)cb.loop, static_cast<unsigned>(cb.priority), (int)active_.finished);
    }

    // Priority check: only block if current is a one-shot still playing.
    // force=true bypasses this so a naturally-ended one-shot can always return
    // to its return_to action regardless of priority.
    bool current_is_oneshot = !cb.loop;
    if (!force && current_is_oneshot && !active_.finished && nb.priority < cb.priority) {
        if (log_enabled) {
            std::fprintf(stderr,
                "[anim-diag][REQUEST] BLOCKED by priority: cur='%s' is a playing one-shot "
                "(priority=%u) and requested '%s' has lower priority=%u\n",
                cb.action.c_str(), static_cast<unsigned>(cb.priority),
                nb.action.c_str(), static_cast<unsigned>(nb.priority));
        }
        return false;
    }

    // Set up blend
    blend_.from_action = cb.action;
    blend_.from_time   = active_.time_sec;
    // Capture the from-binding's start_frame offset so Submit can sample the
    // correct point in the file during the crossfade, just like the to-clip.
    blend_.from_start_offset_sec = 0.f;
    if (active_.binding && active_.binding->start_frame > 0) {
        float fps = active_.binding->fps > 0.f ? active_.binding->fps : 30.f;
        blend_.from_start_offset_sec = active_.binding->start_frame / fps;
    }
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
        std::fprintf(stderr,
            "[anim-diag][REQUEST] APPLIED: current_id_=%u action='%s' "
            "pending_return_=%u ('%s') active_.time_sec reset to 0\n",
            static_cast<unsigned>(current_id_), bindings_[current_id_].action.c_str(),
            static_cast<unsigned>(pending_return_),
            pending_return_ != 0xFF ? bindings_[pending_return_].action.c_str() : "<none>");
    }
    return true;
}

bool AnimController::RequestStateByName(const std::string& action, const char* origin) {
    auto it = action_to_id_.find(action);
    if (it == action_to_id_.end()) return false;
    if (!AnimBindingIsValid(bindings_[it->second])) return false;
    return RequestStateImpl_(it->second, /*force=*/false, origin);
}

void AnimController::Update(float dt, float speed) {
    if (bindings_.empty() || !active_.binding) return;

    // 1. Auto-locomotion — pick the best available locomotion state for current speed
    {
        const std::string& cur = bindings_[current_id_].action;
        bool loco_eligible = (cur == "Idle" || cur == "Walk" || cur == "Run");
        if (loco_eligible) {
            bool hasWalk = HasAction("Walk");
            bool hasRun  = HasAction("Run");

            const char* desired;
            if      (speed > 3.0f  && hasRun)  desired = "Run";
            else if (speed > 0.15f && hasWalk) desired = "Walk";
            else                                desired = "Idle";

            if (cur != desired) {
                if (log_enabled) {
                    std::fprintf(stderr,
                        "[anim-diag][LOCO] speed=%.3f cur='%s' -> requesting '%s'\n",
                        speed, cur.c_str(), desired);
                }
                RequestStateByName(desired, "AnimController::Update/auto-loco");
            }
        } else if (log_enabled) {
            // ⚠️ KEY CHECK (item 4): while cur is NOT Idle/Walk/Run (e.g. stuck on
            // Attack or some corrupted id), auto-locomotion never runs — this is
            // exactly the "preso" symptom if it never flips back to loco_eligible.
            static uint8_t last_logged_non_loco_id = 0xFE;
            if (current_id_ != last_logged_non_loco_id) {
                std::fprintf(stderr,
                    "[anim-diag][LOCO] auto-locomotion SKIPPED: cur='%s' (id=%u) is not "
                    "Idle/Walk/Run — speed=%.3f is being ignored until this returns to a "
                    "loco state\n",
                    cur.c_str(), static_cast<unsigned>(current_id_), speed);
                last_logged_non_loco_id = current_id_;
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
    //
    // FIX 1 (stuck-forever bug, confirmed via [anim-diag] logs: cur_frame climbed
    // to 1190+ on a ~936-frame/31.2s embedded timeline, frame_ended never fired,
    // return_to never ran). A non-loop binding with an invalid end_frame
    // (<= start_frame — this includes the -1 "unset" sentinel some dev-authored
    // bindings ship with, e.g. Hit/Cast/Jump) previously had NO stopping point at
    // all: frame_ended required end_frame > 0, and time_ended required
    // end_frame == -1 AND duration_sec already populated — if duration_sec was
    // still 0 (not yet resolved) neither condition could ever be true, so
    // cur_frame grew without bound every Update() call forever.
    //
    // Every non-loop binding now gets a computed effective end frame so it can
    // NEVER advance unboundedly:
    //   - default_end_frame: start_frame + 1s worth of frames — a short, sane
    //     placeholder so a misconfigured one-shot ends quickly instead of
    //     bleeding into whatever comes next on a shared embedded timeline
    //     (e.g. Hit -> Attack -> Walk -> ... if left unbounded).
    //   - timeline_end_frame: the real clip/timeline length in frames, once
    //     duration_sec is known (SetClipDuration()). Used as an absolute
    //     ceiling — effective end is never later than the actual last frame
    //     of the clip.
    //   effective end = min(default_end_frame, timeline_end_frame when known).
    //
    // This is a stopgap: the dev-facing recommendation is still to configure a
    // real end_frame per binding (see [anim-diag][END-FIX] log below).
    int32_t end_frame = b.end_frame;
    bool    used_fallback_end = false;
    if (!b.loop && end_frame <= b.start_frame) {
        int32_t timeline_end_frame = (b.duration_sec > 0.f)
            ? static_cast<int32_t>(b.duration_sec * effective_fps)
            : -1;
        int32_t default_end_frame = b.start_frame +
            static_cast<int32_t>(kDefaultOneShotFallbackDurationSec * effective_fps);

        int32_t fallback_end = default_end_frame;
        if (timeline_end_frame > b.start_frame && timeline_end_frame < fallback_end)
            fallback_end = timeline_end_frame;

        end_frame = fallback_end;
        used_fallback_end = true;

        if (log_enabled) {
            std::fprintf(stderr,
                "[anim-diag][END-FIX] action='%s' has invalid end_frame=%d (<=start_frame=%d) "
                "— using fallback effective_end_frame=%d (default_%.1fs=%d, timeline_end=%d%s). "
                "⚠️ CONFIG INCOMPLETE: this binding needs a real end_frame set by the dev.\n",
                b.action.c_str(), b.end_frame, b.start_frame, end_frame,
                kDefaultOneShotFallbackDurationSec, default_end_frame, timeline_end_frame,
                timeline_end_frame < 0 ? " (unknown — duration_sec not set yet)" : "");
        }
    }
    bool    frame_ended = (end_frame > 0 && cur_frame >= end_frame);
    bool    time_ended  = (!frame_ended && !used_fallback_end && end_frame == -1 &&
                           b.duration_sec > 0.f && active_.time_sec >= b.duration_sec);

    if (log_enabled && !b.loop) {
        // ⚠️ KEY CHECK (item 2/5): log every update while a one-shot (e.g. Attack)
        // is active, so we can see cur_frame climb and whether it ever crosses
        // end_frame, and what end_frame/duration_sec actually hold at runtime.
        std::fprintf(stderr,
            "[anim-diag][TICK] action='%s' id=%u time_sec=%.4f cur_frame=%d "
            "binding_start=%d binding_end=%d duration_sec=%.4f loop=%d "
            "frame_ended=%d time_ended=%d%s\n",
            b.action.c_str(), static_cast<unsigned>(current_id_), active_.time_sec,
            cur_frame, b.start_frame, end_frame, b.duration_sec, (int)b.loop,
            (int)frame_ended, (int)time_ended,
            (end_frame > 0 && cur_frame > end_frame)
                ? "  <-- ⚠️ cur_frame ALREADY PAST end_frame"
                : "");
    }

    if (frame_ended || time_ended) {
        if (log_enabled) {
            std::fprintf(stderr,
                "[anim-diag][END] end-of-clip DETECTED for action='%s' id=%u "
                "cur_frame=%d end_frame=%d frame_ended=%d time_ended=%d loop=%d "
                "pending_return_=%u ('%s')\n",
                b.action.c_str(), static_cast<unsigned>(current_id_), cur_frame,
                end_frame, (int)frame_ended, (int)time_ended, (int)b.loop,
                static_cast<unsigned>(pending_return_),
                pending_return_ != 0xFF ? bindings_[pending_return_].action.c_str() : "<none>");
        }
        if (b.loop) {
            active_.time_sec         = 0.f;
            active_.last_event_frame = b.start_frame - 1;
        } else {
            // UNCONDITIONAL (bypasses log_enabled — see docs/TECH_DEBT.md
            // #129, "mob morto volta pra Idle" investigation round 2): the
            // exact decision point. Prints regardless of the per-instance
            // log_enabled flag, which defaults false and is never toggled
            // on anywhere in main.cpp — every prior [anim-diag] log in this
            // file was effectively DEAD during normal play, which is why
            // the previous round's fix could not be confirmed by a real
            // log. This one always fires for every one-shot end-of-clip.
            std::fprintf(stderr,
                "[death-term][DECISION] action='%s' is_terminal=%d return_to='%s' "
                "pending_return_=%u priority=%u -> %s\n",
                b.action.c_str(), (int)b.is_terminal, b.return_to.c_str(),
                static_cast<unsigned>(pending_return_),
                static_cast<unsigned>(b.priority),
                pending_return_ != 0xFF ? "RETURN_TO"
                    : (b.is_terminal ? "HOLD_LAST_FRAME(terminal)" : "FALLBACK_TO_IDLE(FIX2)"));
            if (pending_return_ != 0xFF) {
                uint8_t ret     = pending_return_;
                pending_return_ = 0xFF;
                bool applied = RequestStateImpl_(ret, /*force=*/true, "AnimController::Update/RETURN_TO");
                if (log_enabled) {
                    std::fprintf(stderr,
                        "[anim-diag][RETURN_TO] fired: '%s' -> requesting '%s' (id=%u) "
                        "applied=%d ==> now current_id_=%u action='%s' cur_frame_after=?? "
                        "(next Update() will show fresh time_sec=0)\n",
                        b.action.c_str(), bindings_[ret].action.c_str(),
                        static_cast<unsigned>(ret), (int)applied,
                        static_cast<unsigned>(current_id_),
                        bindings_[current_id_].action.c_str());
                }
            } else if (b.is_terminal) {
                // Terminal state BY DESIGN (e.g. Death, priority=99,
                // return_to intentionally empty — see ANIMATION_SYSTEM_SPEC.md
                // and docs/TECH_DEBT.md, mob-death-stuck-in-Idle
                // investigation). This is NOT the misconfiguration FIX 2
                // below guards against — hold the last frame and stop, same
                // as the pre-FIX-2 behavior, but scoped ONLY to bindings that
                // explicitly opted in.
                active_.finished = true;
                std::fprintf(stderr,
                    "[death-term][APPLIED] '%s' is_terminal=true — active_.finished=true, "
                    "current_id_ STAYS at %u ('%s'). If this actor still ends up on Idle "
                    "after this line, the cause is NOT this branch — something else is "
                    "requesting a state change afterward.\n",
                    b.action.c_str(), static_cast<unsigned>(current_id_),
                    bindings_[current_id_].action.c_str());
            } else {
                // FIX 2: never leave the actor with no next state. A one-shot
                // clip with no return_to configured previously just sat
                // "finished" on its last frame forever (also part of the
                // "preso" symptom, independent of the end=-1 bug above — e.g.
                // if return_to_action_id resolved to 0xFF because the return_to
                // name was misspelled or missing from Bind()). Fall back to
                // Idle so the actor always has somewhere to go. Skipped above
                // for is_terminal==true bindings — those have no return_to ON
                // PURPOSE (see the is_terminal branch), not by mistake.
                auto idle_it = action_to_id_.find("Idle");
                if (idle_it != action_to_id_.end() && idle_it->second != current_id_) {
                    std::fprintf(stderr,
                        "[death-term][APPLIED] '%s' is_terminal=false, no return_to — "
                        "FIX2 falling back to 'Idle' (id=%u). If this fired for a Death "
                        "clip, is_terminal did NOT arrive as true on this binding — check "
                        "the [death-term][BIND] log above for what Bind() actually received.\n",
                        b.action.c_str(), static_cast<unsigned>(idle_it->second));
                    RequestStateImpl_(idle_it->second, /*force=*/true, "AnimController::Update/FIX2-fallback");
                } else {
                    active_.finished = true;
                    if (log_enabled) {
                        std::fprintf(stderr,
                            "[anim-diag][END] ⚠️⚠️ no return_to configured for '%s' AND no "
                            "'Idle' action exists in this actor's bindings — marking "
                            "finished=true and staying on this action/frame. This actor "
                            "has no safe fallback state.\n",
                            b.action.c_str());
                    }
                }
            }
        }
    } else if (log_enabled && !b.loop && cur_frame > end_frame && end_frame > 0) {
        // Should be unreachable (frame_ended would have caught it), but logged
        // explicitly in case some other guard is masking frame_ended above.
        std::fprintf(stderr,
            "[anim-diag][END] ⚠️⚠️ cur_frame=%d is PAST end_frame=%d but frame_ended "
            "did NOT trigger end-of-clip handling this Update() call — investigate the "
            "frame_ended condition.\n", cur_frame, end_frame);
    }

    // 5. Advance blend
    if (blend_.active) {
        blend_.elapsed   += dt;
        blend_.from_time += dt * b.speed;  // keep from-anim advancing too
        if (blend_.elapsed >= blend_.duration) {
            blend_.active = false;
        }
    }

    // ⚠️ Post-Update state snapshot (item 3/4): only printed on the frame the
    // end-of-clip branch ran, so we can see exactly what state we ended up in
    // right after Attack was supposed to hand off to Idle.
    if (log_enabled && (frame_ended || time_ended)) {
        const auto& post = bindings_[current_id_];
        bool post_loco_eligible = (post.action == "Idle" || post.action == "Walk" || post.action == "Run");
        std::fprintf(stderr,
            "[anim-diag][POST-END] current_id_=%u action='%s' time_sec=%.4f loop=%d "
            "finished=%d loco_eligible=%d (expect action='Idle' loco_eligible=1 if "
            "return_to worked correctly)\n",
            static_cast<unsigned>(current_id_), post.action.c_str(), active_.time_sec,
            (int)post.loop, (int)active_.finished, (int)post_loco_eligible);
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
    // Offset active_.time_sec by start_frame/fps so ComputeBones samples the
    // correct position in the file for timeline-sliced clips. When start_frame==0
    // (all current Mixamo clips) the offset is 0 and behaviour is identical to before.
    float to_offset = 0.f;
    if (active_.binding && active_.binding->start_frame > 0) {
        float fps = active_.binding->fps > 0.f ? active_.binding->fps : 30.f;
        to_offset = active_.binding->start_frame / fps;
    }
    if (IsBlending()) {
        actor.SubmitBlended(pipeline,
            BlendFromAction(), BlendFromTime() + blend_.from_start_offset_sec,
            CurrentAction(),   CurrentTime()   + to_offset,
            BlendAlpha());
    } else {
        bool loop_flag = active_.binding ? active_.binding->loop : true;
        actor.SubmitAs(CurrentAction(), CurrentTime() + to_offset, loop_flag, pipeline);
    }
}

} // namespace rco::anim
