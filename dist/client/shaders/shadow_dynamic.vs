#version 460 core

// Shadow pass for non-skinned dynamic draws (DynamicDrawRequest in pipeline.cpp).
// Single uniform model matrix per draw call — no SSBO, no gl_DrawID.

layout (location = 0) in vec3 aPos;

uniform mat4 u_lightMatrix;
uniform mat4 u_model;

void main()
{
    gl_Position = u_lightMatrix * u_model * vec4(aPos, 1.0);
}
