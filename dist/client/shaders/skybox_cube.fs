#version 460 core

layout (location = 0) in vec3 vDir;
layout (location = 0) out vec4 fragColor;
layout (binding = 0) uniform samplerCube u_envCube;
uniform float u_skyIntensity;

void main()
{
    vec3 color = texture(u_envCube, normalize(vDir)).rgb * u_skyIntensity;
    // The visible sky uses display-oriented HDR compression. IBL still uses the
    // raw cubemap, so bright HDR skies can light the world without bleaching it.
    color = (color / (vec3(1.0) + color)) * 1.5;
    fragColor = vec4(color, 1.0);
}
