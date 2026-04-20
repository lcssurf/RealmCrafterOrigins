#version 460 core
in  vec3 vWorldPos;
in  vec3 vNormal;
out vec4 FragColor;

uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform vec3 uCamPos;

vec3 HeightColor(float h) {
    vec3 deepwater  = vec3(0.05, 0.15, 0.35);
    vec3 shallowsea = vec3(0.10, 0.30, 0.55);
    vec3 sand       = vec3(0.76, 0.70, 0.50);
    vec3 grass      = vec3(0.25, 0.48, 0.18);
    vec3 darkgrass  = vec3(0.18, 0.35, 0.12);
    vec3 rock       = vec3(0.45, 0.42, 0.38);
    vec3 snow       = vec3(0.92, 0.95, 0.98);

    if      (h < -2.0) return deepwater;
    else if (h < -0.5) return mix(deepwater,  shallowsea, (h + 2.0) / 1.5);
    else if (h <  0.5) return mix(shallowsea, sand,       (h + 0.5) / 1.0);
    else if (h <  2.0) return mix(sand,       grass,      (h - 0.5) / 1.5);
    else if (h <  5.0) return mix(grass,      darkgrass,  (h - 2.0) / 3.0);
    else if (h < 10.0) return mix(darkgrass,  rock,       (h - 5.0) / 5.0);
    else               return mix(rock,       snow,       clamp((h - 10.0) / 5.0, 0.0, 1.0));
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uSunDir);

    vec3  base = HeightColor(vWorldPos.y);
    float diff = max(dot(N, L), 0.0);

    float specStr = (vWorldPos.y < 0.0) ? 0.3 : 0.02;
    vec3  V    = normalize(uCamPos - vWorldPos);
    vec3  R    = reflect(-L, N);
    float spec = pow(max(dot(V, R), 0.0), 32.0) * specStr;

    vec3 color = base * (uAmbient + uSunColor * diff) + uSunColor * spec;

    float dist = length(uCamPos - vWorldPos);
    float fog  = clamp((dist - 150.0) / 200.0, 0.0, 1.0);
    color = mix(color, vec3(0.60, 0.75, 0.90), fog);

    FragColor = vec4(color, 1.0);
}
