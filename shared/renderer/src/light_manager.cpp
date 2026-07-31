#include "rco/renderer/light_manager.h"
#include "rco/renderer/pipeline.h"

namespace rco::renderer {

void LightManager::SubmitAll(Pipeline& pipeline) const {
    for (const auto& l : lights_) {
        // Pipeline::AddPointLight/AddSpotLight/AddDirectionalLocalLight all
        // bake color*intensity into a single vec4 diffuse term (see
        // PointLight::diffuse in light.h) — no separate intensity uniform
        // downstream, so it's folded in here for every type.
        switch (l.type) {
            case LightType::Spot:
                pipeline.AddSpotLight(l.pos, l.color * l.intensity, l.radius,
                                       l.direction, l.coneOuterDeg, l.coneInnerDeg);
                break;
            case LightType::Directional:
                pipeline.AddDirectionalLocalLight(l.pos, l.color * l.intensity, l.radius,
                                                   l.direction);
                break;
            case LightType::Point:
            default:
                pipeline.AddPointLight(l.pos, l.color * l.intensity, l.radius);
                break;
        }
    }
}

} // namespace rco::renderer
