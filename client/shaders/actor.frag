#version 460 core
in  vec3 vWorldPos;
in  vec3 vNormal;
in  vec2 vUV;
out vec4 FragColor;

uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform vec3 uCamPos;
uniform vec3 uBaseColor;

void main() {
    vec3 N    = normalize(vNormal);
    vec3 L    = normalize(uSunDir);
    float diff = max(dot(N, L), 0.0);

    vec3 V    = normalize(uCamPos - vWorldPos);
    vec3 R    = reflect(-L, N);
    float spec = pow(max(dot(V, R), 0.0), 32.0) * 0.15;

    vec3 color = uBaseColor * (uAmbient + uSunColor * diff) + uSunColor * spec;

    // Distance fog matching terrain shader
    float dist = length(uCamPos - vWorldPos);
    float fog  = clamp((dist - 150.0) / 200.0, 0.0, 1.0);
    color = mix(color, vec3(0.60, 0.75, 0.90), fog);

    FragColor = vec4(color, 1.0);
}
