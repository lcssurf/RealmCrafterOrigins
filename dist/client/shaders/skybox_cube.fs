#version 460 core

layout (location = 0) in vec3 vDir;
layout (location = 0) out vec4 fragColor;
layout (binding = 0) uniform samplerCube u_envCube;

void main()
{
    vec3 color = texture(u_envCube, normalize(vDir)).rgb;
    fragColor = vec4(color, 1.0);
}
