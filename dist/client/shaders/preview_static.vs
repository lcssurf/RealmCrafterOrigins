#version 460 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec3 aTangent;

uniform mat4 u_viewProj;
uniform mat4 u_model;
uniform vec2 u_uvOffset;   // diagnostic: lets the GUE shift UVs interactively
uniform vec2 u_uvScale;

out vec2  vTexCoord;
out vec3  vWorldNormal;

void main()
{
    vTexCoord    = aTexCoord * u_uvScale + u_uvOffset;
    vWorldNormal = normalize(mat3(transpose(inverse(u_model))) * aNormal);
    gl_Position  = u_viewProj * u_model * vec4(aPos, 1.0);
}
