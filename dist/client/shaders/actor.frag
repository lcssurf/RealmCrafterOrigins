#version 460 core
in  vec3 vWorldPos;
in  vec2 vUV;
in  mat3 vTBN;
out vec4 FragColor;

uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform vec3 uCamPos;

// PBR material
uniform sampler2D uAlbedo;          // unit 0 — sRGB base color
uniform sampler2D uNormalMap;       // unit 1 — linear tangent-space normals
uniform sampler2D uORM;             // unit 2 — linear: R=AO, G=Roughness, B=Metallic

uniform vec3  uAlbedoFactor;        // multiplied with albedo texture
uniform float uRoughnessFactor;     // multiplied with roughness channel
uniform float uMetallicFactor;      // multiplied with metallic channel

// ---------------------------------------------------------------------------
// Cook-Torrance BRDF
// ---------------------------------------------------------------------------
const float PI = 3.14159265359;

// GGX/Trowbridge-Reitz normal distribution
float D_GGX(float NdotH, float a2) {
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Schlick-GGX geometry term (one side)
float G_Schlick(float NdotX, float k) {
    return NdotX / (NdotX * (1.0 - k) + k);
}

// Smith geometry — both view and light sides
float G_Smith(float NdotL, float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return G_Schlick(NdotL, k) * G_Schlick(NdotV, k);
}

// Fresnel-Schlick
vec3 F_Schlick(float VdotH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

// Evaluate direct lighting contribution from one directional light
vec3 DirectBRDF(vec3 albedo, float metallic, float roughness,
                vec3 N, vec3 V, vec3 L, vec3 lightColor) {
    vec3  H     = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Dielectric F0 = 0.04; metals use albedo as F0
    vec3  F0   = mix(vec3(0.04), albedo, metallic);
    float a    = roughness * roughness;
    float a2   = a * a;

    float D = D_GGX(NdotH, a2);
    float G = G_Smith(NdotL, NdotV, roughness);
    vec3  F = F_Schlick(VdotH, F0);

    vec3 specular = (D * G * F) / max(4.0 * NdotL * NdotV, 0.001);

    // Energy-conserving diffuse: metals have no diffuse
    vec3 kD      = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * lightColor * NdotL;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main() {
    // --- Sample textures ---
    vec4  albedaTex  = texture(uAlbedo,    vUV); // GL_SRGB8_ALPHA8 → already linear
    vec3  albedo     = albedaTex.rgb * uAlbedoFactor;
    vec3  orm        = texture(uORM,       vUV).rgb;
    float ao         = orm.r;
    float roughness  = clamp(orm.g * uRoughnessFactor, 0.04, 1.0);
    float metallic   = clamp(orm.b * uMetallicFactor,  0.0,  1.0);

    // --- Normal mapping ---
    vec3 tn = texture(uNormalMap, vUV).rgb * 2.0 - 1.0;
    vec3 N  = normalize(vTBN * tn);
    vec3 V  = normalize(uCamPos - vWorldPos);
    vec3 L  = normalize(uSunDir);

    // --- Direct lighting ---
    vec3 color = DirectBRDF(albedo, metallic, roughness, N, V, L, uSunColor);

    // --- Ambient (occlusion-weighted) ---
    color += albedo * uAmbient * ao;

    // --- Tone map (Reinhard) + fog ---
    color = color / (color + vec3(1.0));
    float fog  = clamp((length(uCamPos - vWorldPos) - 150.0) / 200.0, 0.0, 1.0);
    color = mix(color, vec3(0.60, 0.75, 0.90), fog);

    FragColor = vec4(color, albedaTex.a);
}
