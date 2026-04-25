#version 460
layout(location = 0) in vec3 a_position;

uniform mat4  u_viewProj;
uniform vec3  u_brushPos;
uniform float u_brushRadius;

void main() {
    // a_position.xz is unit circle in [-1, 1]; y = 0.
    vec3 wp = vec3(u_brushPos.x + a_position.x * u_brushRadius,
                   u_brushPos.y + 0.2,   // lift to avoid z-fighting with terrain
                   u_brushPos.z + a_position.z * u_brushRadius);
    gl_Position = u_viewProj * vec4(wp, 1.0);
}
