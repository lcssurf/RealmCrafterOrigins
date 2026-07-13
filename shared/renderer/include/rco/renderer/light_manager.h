#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace rco::renderer {

class Pipeline;

// One static point light (torch/lantern) as received from the server via
// PZoneLights — see server/internal/world/frame.go LightsPayload and
// tools/gue/src/zone_scene.h ZLight (the authoring side, Zone editor).
struct PointLightEntry {
    glm::vec3 pos{0.f};
    glm::vec3 color{1.f};
    float     intensity = 1.f;
    float     radius    = 5.f;
};

// Holds the current area's static point lights and resubmits them into the
// deferred pipeline every frame.
//
// IMPORTANT: Pipeline::Begin() clears its internal light list every frame
// (see localLights_ in pipeline.cpp) — point lights are NOT "fire and
// forget" like a one-time AddPointLight() call. This is the pipeline's
// existing, correct design (it mirrors how DynamicDrawRequest/SubmitDynamic
// etc. all work: submit-per-frame, not submit-once). LightManager exists
// specifically to keep re-adding the area's lights every single frame for as
// long as they're part of the current area, by calling SubmitAll() once per
// frame between Pipeline::Begin() and Pipeline::End().
//
// Phase 1 (this class): static lights only, one list per area, replaced
// wholesale on PZoneLights (initial area entry + every portal/area change).
// Phase 2 (not implemented here — see doc/TECH_DEBT.md): dynamic lights
// attached to skill/FX ParticleSystem::Emitter instances, reusing this same
// SubmitAll()-into-Pipeline::AddPointLight() mechanism.
class LightManager {
public:
    // Replaces the full light list — call when a PZoneLights packet arrives.
    void SetLights(std::vector<PointLightEntry> lights) { lights_ = std::move(lights); }

    // Drops all lights — call on area/logout/disconnect so a stray leftover
    // light from the previous area can't render for a frame or two before
    // the new area's PZoneLights packet (if any) arrives.
    void Clear() { lights_.clear(); }

    size_t Count() const { return lights_.size(); }

    // Call once per frame, after Pipeline::Begin() and before Pipeline::End()
    // — matches every other Submit*/AddPointLight caller's frame contract.
    void SubmitAll(Pipeline& pipeline) const;

private:
    std::vector<PointLightEntry> lights_;
};

} // namespace rco::renderer
