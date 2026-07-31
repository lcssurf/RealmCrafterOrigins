#pragma once
#include <glm/glm.hpp>

namespace rco::renderer {

struct PointLight {
    glm::vec4 diffuse       {};
    glm::vec4 position      {};
    float     linear        {};
    float     quadratic     {};
    float     radiusSquared {};
    float     _padding      {};

    // Spot/Directional support — additive, appended after the original
    // fields above so the struct's first 48 bytes (and its SSBO layout for
    // every Point light already in flight) are byte-identical to before.
    // A default-constructed PointLight{} zero-inits both of these, and
    // spotParams.z==0 IS the Point light_type — so AddPointLight() below
    // needs zero changes to keep producing exactly today's Point behavior.
    glm::vec4 direction  {}; // xyz: unit "aim" direction (Spot/Directional only, unused for Point). w unused.
    glm::vec4 spotParams {}; // x=cos(outer cone half-angle) y=cos(inner cone half-angle) z=light_type(0=Point,1=Spot,2=Directional) w unused

    float CalcRadiusSquared(float epsilon) const;
};

struct DirLight {
    glm::vec3 diffuse   {};
    glm::vec3 direction {};
};

glm::mat4 MakeLightMatrix(const DirLight& light, glm::vec3 eye,
                          glm::vec2 dim, glm::vec2 depthRange);

} // namespace rco::renderer
