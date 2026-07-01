#version 460 core

in vec2  vTexCoord;
in vec3  vWorldNormal;

out vec4 fragColor;

uniform sampler2D u_albedo;
uniform bool      u_hasAlbedo;
uniform vec3      u_albedoFactor;   // tint when no texture

uniform vec3  u_sunDir;             // world-space, normalised, pointing TO light
uniform vec3  u_sunColor;           // sun RGB * intensity
uniform float u_ambientStrength;    // ambient fill (0-1)

uniform bool  u_blackCutout;
uniform float u_blackCutoutThreshold;

void main()
{
    // Texture is stored as GL_SRGB8_ALPHA8: sampling returns linear values.
    vec3 baseColor = u_hasAlbedo
        ? texture(u_albedo, vTexCoord).rgb
        : u_albedoFactor;

    vec3 N = normalize(vWorldNormal);
    float ndotl = max(dot(N, -normalize(u_sunDir)), 0.0);

    vec3 color = baseColor * (u_ambientStrength + ndotl * u_sunColor);

    if (u_blackCutout) {
        float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
        if (lum < u_blackCutoutThreshold) discard;
    }

    // Gamma-encode for sRGB display (FBO is GL_RGBA8, no automatic conversion).
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    fragColor = vec4(color, 1.0);
}
