#version 460 core
in  vec3 vDir;
out vec4 FragColor;

uniform vec3 uSunDir;
uniform vec3 uSkyTop;
uniform vec3 uSkyHorizon;
uniform vec3 uSunColor;

void main() {
    vec3 dir = normalize(vDir);
    float t  = clamp(dir.y * 1.5 + 0.1, 0.0, 1.0);
    vec3  sky = mix(uSkyHorizon, uSkyTop, t);

    float sunDot = dot(dir, normalize(uSunDir));
    float sun    = smoothstep(0.998, 1.0, sunDot);
    float halo   = pow(max(sunDot, 0.0), 16.0) * 0.3;

    FragColor = vec4(sky + uSunColor * (sun + halo), 1.0);
}
