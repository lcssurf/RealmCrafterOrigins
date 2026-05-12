#version 460 core

in vec3 vLocalPos;
layout (binding = 0) uniform sampler2D u_equirect;

layout (location = 0) out vec4 fragColor;

const vec2 invAtan = vec2(0.1591, 0.3183);

vec2 sampleSphericalMap(vec3 v)
{
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

void main()
{
    vec2 uv = sampleSphericalMap(normalize(vLocalPos));
    vec3 c = texture(u_equirect, uv).rgb;
    // Guard against invalid/overflowed HDR texels around strong sun hotspots.
    if (c.r != c.r) c.r = 0.0;
    if (c.g != c.g) c.g = 0.0;
    if (c.b != c.b) c.b = 0.0;
    c = clamp(c, vec3(0.0), vec3(64000.0));
    fragColor = vec4(c, 1.0);
}
