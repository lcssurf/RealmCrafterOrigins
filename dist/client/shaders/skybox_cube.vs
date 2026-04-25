#version 460 core

layout (location = 0) out vec3 vDir;

uniform mat4 u_invViewProj;
uniform vec3 u_camPos;

vec2 createTri(uint id)
{
    uint b = 1u << id;
    return vec2((0x4u & b) != 0u, (0x1u & b) != 0u);
}

void main()
{
    vec2 pos = createTri(uint(gl_VertexID));
    vec4 ndc = vec4(pos * 4.0 - 1.0, 1.0, 1.0);
    gl_Position = ndc;

    vec4 world = u_invViewProj * ndc;
    vDir = (world.xyz / world.w) - u_camPos;
}
