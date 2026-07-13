#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

namespace rco::renderer {

// Hugo Elias ripple height-field simulation, reacting to the local player.
// Two small GL_R32F ping-pong textures updated every frame via a classic
// verlet-style neighbor-average recurrence (see dist/client/shaders/
// ripple_update.fs for the exact formula and the same-texel self-read
// caveat). The buffer represents a small, player-FOLLOWING window of world
// space (windowCenter_/windowSize_), not a fixed UV range and not the
// water plane's own UV — see WorldToUV().
//
// Phase (a) (done): standalone buffer + debug visualization only.
// Phase (b) (this): dynamic world window + automatic player-position
// stamping + sampled by water.vs/water.fs (see WaterManager::Render and
// WaterEntry::Contains for the "only the water the player is standing in"
// gating — this class itself has no notion of "which water", it's purely
// the height-field simulation + world<->UV mapping).
class RippleSim {
public:
    // Creates the two ping-pong textures/FBOs + a dummy VAO for the
    // fullscreen-triangle draws. `size` is the buffer resolution (square,
    // e.g. 128 or 256) — call once after the GL context is ready.
    // `windowSize` is the world-space width/depth (in world units) the
    // buffer represents around its center — only an INITIAL/fallback value
    // used until the first UpdateWindow() call (which takes windowSize as
    // a live parameter every frame — see below); 7.0 is a human-scale
    // placeholder (a person is ~1.8 units tall) in case UpdateWindow() is
    // ever skipped for a frame.
    void Init(int size = 128, float windowSize = 7.0f);
    void Shutdown();

    // Recenters the world window on `playerXZ` — but only when the player
    // has drifted far enough from the CURRENT center (hysteresis), not
    // every frame: recenters when distance > 0.25 * windowSize_. On
    // recenter, the buffer contents ARE shifted (not discarded) — see
    // Update()'s shift pass, which runs the frame a recenter is flagged
    // here, BEFORE that frame's normal Hugo Elias propagation. This keeps
    // existing ripples attached to their world position instead of jumping/
    // disappearing every time the window follows the player (an earlier
    // version skipped the shift entirely, which looked fine for a single
    // debug stamp but broke continuous-walking trails — see doc/TECH_DEBT.md
    // #118). Call once per frame BEFORE Update() so WorldToUV() reflects
    // the current frame's window when the stamp UV is computed, and so a
    // pending shift (if any) is queued before Update() consumes it.
    //
    // `windowSize` is refreshed EVERY call (not gated by hysteresis — it's
    // a scale, not a position). Callers derive it from the player's live
    // ModelHeight() (see main.cpp) so a runtime model/scale change
    // (equipment, buff, etc) is picked up automatically without needing
    // dirty-tracking or a separate setter. Values <= 0.01 are ignored
    // (keeps the previous windowSize_) — guards against WorldToUV()
    // dividing by zero/near-zero if the caller ever passes a degenerate
    // height (e.g. actor not loaded — though Actor::ModelHeight() already
    // has its own 1.8 fallback and should never actually reach 0 here;
    // this is defense in depth, not the primary safeguard).
    void UpdateWindow(glm::vec2 playerXZ, float windowSize);

    // Converts a world-space XZ position into [0,1] UV space of the
    // CURRENT window (windowCenter_/windowSize_) — used both for the C++
    // stamp position (Update()'s stampUV) and replicated (same formula) in
    // water.vs for sampling the ripple displacement/normal at each vertex.
    glm::vec2 WorldToUV(glm::vec3 worldPos) const {
        return (glm::vec2(worldPos.x, worldPos.z) - windowCenter_) / windowSize_ + 0.5f;
    }

    glm::vec2 WindowCenter() const { return windowCenter_; }
    float     WindowSize()   const { return windowSize_; }

    // Runs one Hugo Elias update step: reads the "current" buffer (4-
    // neighbor average) and the "previous" buffer (same-texel subtraction
    // term — see ripple_update.fs), writes the result into the buffer that
    // was "previous" (it's no longer needed after this read), then that
    // buffer becomes "current" for the next call. If `stamp` is true, a
    // soft circular blob is added into the freshly-computed height at
    // `stampUV` in [0,1] buffer space in the SAME pass (after the
    // propagation math — see ripple_update.fs's comment on ordering).
    // `stampRadius` is in UV space [0,1] — already a fraction of whatever
    // windowSize_ currently is (WorldToUV normalizes by windowSize_ before
    // any radius comparison happens), so it does NOT need rescaling when
    // windowSize_ changes: 0.07 always means "7% of the window". Default
    // `stampStrength` calibrated down from an initial 1.0 (was close to
    // saturating the buffer at the stamp point) — see main.cpp's
    // kRippleStampRadius/kRippleStampStrength for the call site that
    // should be tuned first if this still needs adjusting. `damping`
    // default lowered from 0.985 to 0.95 (was decaying too slowly — ~46
    // frames to halve at 0.985 vs ~14 at 0.95 — see main.cpp's
    // kRippleDamping, which callers should pass explicitly rather than
    // relying on this default, same as the other tuning knobs).
    //
    // If UpdateWindow() flagged a recenter this frame, Update() runs a
    // separate shift pass FIRST (see ShiftBuffer(), private) that slides
    // both ping-pong buffers' contents by the recenter's texel offset, so
    // ripples already in the buffer stay attached to their world position
    // instead of jumping to a stale/wrong texel. Only 3 small (size_ x
    // size_, e.g. 128x128) fullscreen-triangle draws, and ONLY on a
    // recenter frame — hysteresis (see UpdateWindow) keeps recenters rare
    // (roughly every 15-25 frames while continuously walking, never every
    // frame), so this is not a per-frame cost.
    void Update(bool stamp, glm::vec2 stampUV = {0.5f, 0.5f},
                float stampRadius = 0.07f, float stampStrength = 0.4f,
                float damping = 0.95f);

    // Debug-only: draws the current height buffer as a small quad in the
    // screen's top-left corner (reuses Shader::shaders["fstexture"], the
    // existing fullscreen-triangle + texture.fs pass, restricted to a
    // small sub-viewport). Call once per frame, gated by the caller's own
    // debug toggle (main.cpp wires this to F9). No-op if Init() wasn't
    // called or the "fstexture" shader isn't registered.
    void DebugDraw(int viewportW, int viewportH) const;

    GLuint CurrentTexture() const { return tex_[cur_]; }
    int    Size()           const { return size_; }

    // Sampler object callers (WaterManager::Render) must bind to whatever
    // texture unit they bind CurrentTexture() to, via glBindSampler(unit,
    // BorderSampler()) — GL_CLAMP_TO_BORDER with border=(0,0,0,0), so
    // sampling outside the player's window reads a real zero. This is
    // DELIBERATELY separate from the texture object's own baked-in wrap
    // mode (GL_CLAMP_TO_EDGE, set at texture creation — see Init()), which
    // is what ripple_update.fs's internal 4-neighbor reads fall back to
    // (no sampler bound during Update(), or rather Update() explicitly
    // binds its own edge-clamp sampler — see .cpp). Putting CLAMP_TO_BORDER
    // directly on the texture object (an earlier attempt) affected BOTH
    // uses identically, since a texture's own glTextureParameteri state is
    // what every sampling falls back to when no sampler object overrides
    // it — that broke the simulation itself (border=0 as a hugely
    // absorbing wall right at the edge of a small 128px buffer kills the
    // wave much faster than intended). Same physical texture, two
    // DIFFERENT sampler objects for the two different consumers.
    GLuint BorderSampler() const { return samplerBorder_; }

private:
    // Slides both ping-pong buffers' contents by `offsetUV` (UV-space
    // fraction of the window) — called by Update() when UpdateWindow()
    // flagged a recenter this frame. Needs a THIRD scratch texture
    // (texShift_/fboShift_) because a straight "shift A into B" would
    // require B's original content to still be read afterward (for its own
    // shift) — 3 passes: (1) scratch = shifted(tex_[cur_]) [reads cur_
    // before it's touched], (2) tex_[cur_] = shifted(tex_[dst]) [safe to
    // overwrite cur_ now that scratch has its data], (3) tex_[dst] =
    // scratch [plain copy, already shifted]. After these 3 passes, the
    // physical slot that WAS `dst` holds the correctly-shifted "current"
    // data and the slot that WAS `cur_` holds the correctly-shifted
    // "previous" data — so cur_ is flipped (cur_ = dst) at the end to match
    // the new physical layout, exactly like the normal ping-pong swap in
    // Update() does for its own pass.
    void ShiftBuffer(glm::vec2 offsetUV);

    int    size_ = 0;
    GLuint tex_[2] = {0, 0};
    GLuint fbo_[2] = {0, 0};
    GLuint texShift_ = 0;  // scratch buffer for ShiftBuffer()'s 3-pass shift, same size/format as tex_[]
    GLuint fboShift_ = 0;
    GLuint dummyVao_ = 0;  // fullscreen-triangle draws need a bound VAO (core profile) but no vertex data
    int    cur_ = 0;       // index into tex_/fbo_ of the "current" (most recently completed) buffer
    GLuint samplerEdge_   = 0;  // GL_CLAMP_TO_EDGE — bound internally during Update() for the simulation's own neighbor reads
    GLuint samplerBorder_ = 0;  // GL_CLAMP_TO_BORDER, border=0 — exposed via BorderSampler() for water.vs/water.fs's window-relative sampling

    bool      pendingShift_ = false;       // set by UpdateWindow() on a hysteresis recenter, consumed by Update()
    glm::vec2 pendingShiftOffsetUV_{0.0f}; // ShiftBuffer()'s u_offsetUV for the pending shift

    glm::vec2 windowCenter_{0.0f};
    float     windowSize_ = 7.0f;
    bool      windowInitialized_ = false;  // first UpdateWindow() call snaps instead of waiting for hysteresis
};

} // namespace rco::renderer
