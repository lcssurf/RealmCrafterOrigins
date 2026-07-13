#include "rco/renderer/light_manager.h"
#include "rco/renderer/pipeline.h"

namespace rco::renderer {

void LightManager::SubmitAll(Pipeline& pipeline) const {
    for (const auto& l : lights_) {
        // Pipeline::AddPointLight bakes color*intensity into a single
        // vec4 diffuse term (see PointLight::diffuse in light.h) — there's
        // no separate intensity uniform downstream, so it's folded in here.
        pipeline.AddPointLight(l.pos, l.color * l.intensity, l.radius);
    }
}

} // namespace rco::renderer
