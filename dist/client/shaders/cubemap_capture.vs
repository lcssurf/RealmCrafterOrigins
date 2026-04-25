#version 460 core

layout (location = 0) in vec3 aPos;

uniform mat4 u_view;
uniform mat4 u_proj;

out vec3 vLocalPos;

void main()
{
    vLocalPos = aPos;
    gl_Position = u_proj * u_view * vec4(aPos, 1.0);
}
