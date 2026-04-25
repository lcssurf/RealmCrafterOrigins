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
    fragColor = vec4(texture(u_equirect, uv).rgb, 1.0);
}
