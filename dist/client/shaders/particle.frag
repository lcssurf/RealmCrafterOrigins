#version 460 core

in  vec4 vColor;
out vec4 fragColor;

void main() {
    // Round soft particle using point coord.
    vec2  c    = gl_PointCoord * 2.0 - 1.0;
    float dist = dot(c, c);
    if (dist > 1.0) discard;
    float alpha = (1.0 - dist * dist) * vColor.a;
    fragColor = vec4(vColor.rgb, alpha);
}
