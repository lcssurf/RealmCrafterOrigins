#version 460 core

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec4  aColor;
layout(location = 2) in float aSize;

uniform mat4 uView;
uniform mat4 uProj;

out vec4 vColor;

void main() {
    vColor       = aColor;
    gl_Position  = uProj * uView * vec4(aPos, 1.0);
    gl_PointSize = aSize;
}
