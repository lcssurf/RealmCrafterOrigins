#version 460 core
#include "common.h"

#define SHADOW_METHOD_PCF 0
#define SHADOW_METHOD_VSM 1
#define SHADOW_METHOD_ESM 2
#define SHADOW_METHOD_MSM 3

layout (location = 0) in vec2 vTexCoord;

// G-buffer
layout (binding = 0) uniform sampler2D gNormal;
layout (binding = 1) uniform sampler2D gAlbedo;
layout (binding = 2) uniform sampler2D gRMA;
layout (binding = 3) uniform sampler2D gDepth;

// AO + shadow
layout (binding = 4) uniform sampler2D ambientOcclusionTexture;
layout (binding = 5) uniform sampler2D filteredShadow;
// Raw shadow depth — used only by the debug visualisation (mode 11).
layout (binding = 9) uniform sampler2D shadowDepthRaw;

// IBL — cubemap suite (split-sum approximation)
layout (binding = 6) uniform samplerCube irradianceCube;
layout (binding = 7) uniform samplerCube prefilterCube;
layout (binding = 8) uniform sampler2D   brdfLUT;

// uniforms — locations omitted to avoid mat4 overlap; resolved by name.
uniform ivec2 u_screenSize;
uniform vec3  u_viewPos;
uniform mat4  u_lightMatrix;
uniform mat4  u_invViewProj;
uniform vec3  u_globalLight_diffuse;
uniform vec3  u_globalLight_direction;
uniform int   u_shadowMethod;
uniform float u_C;
uniform float u_lightBleedFix;
uniform float u_directScale;
uniform float u_ambientScale;
uniform float u_characterShadowLift;
uniform float u_characterRimStrength;
uniform float u_characterRimExponent;
uniform float u_characterMinNdotL;
uniform float u_characterAmbientBoost;
uniform float u_worldShadowLift;
uniform float u_iblIntensity;
uniform float u_flatAmbient;
uniform float u_worldMinNdotL;
uniform float u_albedoMinLuma;
uniform float u_albedoLiftStrength;
uniform float u_specularScale;
uniform int   u_debugMode;

layout (location = 0) out vec4 fragColor;

const float PI = 3.14159265359;
const float MAX_REFLECTION_LOD = 4.0;

// ---------------------------------------------------------------------------
// PBR helpers (Cook-Torrance + Schlick + GGX)
// ---------------------------------------------------------------------------
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
vec3 liftLegacyAlbedo(vec3 color) {
    float minLuma = clamp(u_albedoMinLuma, 0.0, 1.0);
    float strength = clamp(u_albedoLiftStrength, 0.0, 1.0);
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    if (luma >= minLuma || strength <= 0.0) return color;

    // Scale color toward a minimum luminance instead of adding white. This
    // keeps hue/detail better than a flat brightness boost on legacy assets.
    float safeLuma = max(luma, 0.025);
    vec3 lifted = clamp(color * (minLuma / safeLuma), 0.0, 1.0);
    if (luma < 0.025) lifted = vec3(minLuma);
    return mix(color, lifted, strength);
}
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// ---------------------------------------------------------------------------
// Shadow sampling
// ---------------------------------------------------------------------------
vec3 ShadowTexCoord(vec4 lightSpacePos) {
    vec3 p = lightSpacePos.xyz / lightSpacePos.w;
    if (p.z > 1.0) return vec3(1.0);
    return p * 0.5 + 0.5;
}
float linstep(float lo, float hi, float v) { return clamp((v - lo) / (hi - lo), 0.0, 1.0); }
float ReduceLightBleeding(float p, float amount) { return linstep(amount, 1.0, p); }
float Chebyshev(vec2 m, float t) {
    float p = (t <= m.x) ? 1.0 : 0.0;
    float variance = max(m.y - m.x * m.x, 0.000001);
    float d = m.x - t;
    float p_max = ReduceLightBleeding(variance / (variance + d * d), u_lightBleedFix);
    return max(p, p_max);
}
float ShadowVSM(vec4 lsp) {
    vec3 c = ShadowTexCoord(lsp);
    return Chebyshev(texture(filteredShadow, c.xy).xy, c.z);
}
float ShadowESM(vec4 lsp) {
    vec3 c = ShadowTexCoord(lsp);
    float ld = texture(filteredShadow, c.xy).x;
    return clamp(ld * exp(-u_C * c.z), 0.0, 1.0);
}
// 3x3 PCF over the raw shadow depth — robust to sparse occluders (most of the
// shadow map is empty / depth=1 because only a handful of dynamic actors
// actually project shadows). ESM with C=80 turns the empty regions into
// near-infinite ld values that drown any real occluders during blur.
float ShadowPCF(vec4 lsp) {
    vec3 c = ShadowTexCoord(lsp);
    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0) return 1.0;
    if (c.z >= 1.0) return 1.0;
    float bias = 0.0015;
    vec2 texel = 1.0 / vec2(textureSize(shadowDepthRaw, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float occluder = texture(shadowDepthRaw,
                                     c.xy + vec2(x, y) * texel).r;
            sum += (c.z - bias > occluder) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}
float Shadow(vec4 lsp) {
    if (u_shadowMethod == SHADOW_METHOD_VSM) return ShadowVSM(lsp);
    if (u_shadowMethod == SHADOW_METHOD_ESM) return ShadowESM(lsp);
    return ShadowPCF(lsp);  // PCF — used by SHADOW_METHOD_PCF (0)
}

void main()
{
    float depth = texture(gDepth, vTexCoord).r;
    if (depth >= 1.0) discard;

    vec3 albedo   = texture(gAlbedo, vTexCoord).rgb;
    vec3 litAlbedo = liftLegacyAlbedo(albedo);
    vec3 vPos     = WorldPosFromDepth(depth, u_screenSize, u_invViewProj);
    vec2 octN     = texture(gNormal, vTexCoord).xy;
    vec3 N        = normalize(oct_to_float32x3(octN));
    vec4 RMA      = texture(gRMA, vTexCoord);
    float roughness = clamp(RMA.r, 0.04, 1.0);
    float metallic  = clamp(RMA.g, 0.0, 1.0);
    float matAO     = RMA.b;
    float charMask  = clamp(RMA.a, 0.0, 1.0);
    float ssao      = texture(ambientOcclusionTexture, vTexCoord).r;
    float ao        = matAO * ssao;

    // -- Debug visualisation modes (set via Pipeline::SetDebugMode) --
    if (u_debugMode == 1) { fragColor = vec4(albedo, 1.0); return; }
    if (u_debugMode == 2) { fragColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (u_debugMode == 3) { fragColor = vec4(vec3(depth), 1.0); return; }
    if (u_debugMode == 4) { fragColor = vec4(vec3(ao), 1.0); return; }
    // Mode 11: visualise the RAW shadow depth (not the ESM-exp version) at
    // this fragment's light-space UV. Sample is in [0,1] so geometry shows up
    // as darker silhouettes against a white (depth=1) background.
    if (u_debugMode == 11) {
        vec4 lsp = u_lightMatrix * vec4(vPos, 1.0);
        vec3 lpu = lsp.xyz / lsp.w;
        lpu = lpu * 0.5 + 0.5;
        if (lpu.x < 0.0 || lpu.x > 1.0 || lpu.y < 0.0 || lpu.y > 1.0) {
            fragColor = vec4(1, 0, 1, 1);
            return;
        }
        float sd = texture(shadowDepthRaw, lpu.xy).r;
        fragColor = vec4(vec3(sd), 1.0);
        return;
    }
    vec3 V = normalize(u_viewPos - vPos);
    vec3 R = reflect(-V, N);

    // Fresnel base reflectance — 0.04 dielectric, albedo for metals.
    vec3 F0 = mix(vec3(0.04), litAlbedo, metallic);

    // ---- Direct lighting (single directional sun) ----
    vec3 L = normalize(-u_globalLight_direction);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);

    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator   = D * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
    vec3 specular = (numerator / denominator) * u_specularScale;

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    float worldMinNdotL = (1.0 - charMask) * max(u_worldMinNdotL, 0.0);
    float NdotLReadable = max(NdotL, max(worldMinNdotL, charMask * max(u_characterMinNdotL, 0.0)));
    vec3 directColor = (kD * litAlbedo / PI + specular) * u_globalLight_diffuse * NdotLReadable * u_directScale;

    vec4 lsp = u_lightMatrix * vec4(vPos, 1.0);
    float shadow = (NdotLReadable > 0.0) ? Shadow(lsp) : 0.0;
    float worldShadowLifted = mix(shadow, 1.0, clamp(u_worldShadowLift, 0.0, 1.0));
    float charShadowLifted = mix(shadow, 1.0, clamp(u_characterShadowLift, 0.0, 1.0));
    shadow = mix(worldShadowLifted, charShadowLifted, charMask);

    if (u_debugMode == 5) { fragColor = vec4(vec3(shadow), 1.0); return; }
    if (u_debugMode == 7) { fragColor = vec4(vec3(NdotL), 1.0); return; }
    if (u_debugMode == 8) { fragColor = vec4(litAlbedo * NdotL, 1.0); return; }
    if (u_debugMode == 10) { fragColor = vec4(directColor, 1.0); return; }

    vec3 Lo = directColor * shadow;
    float NdotV = max(dot(N, V), 0.0);
    float rim = pow(clamp(1.0 - NdotV, 0.0, 1.0), max(u_characterRimExponent, 1.0));
    Lo += litAlbedo * u_globalLight_diffuse * rim * u_characterRimStrength * charMask;

    // ---- Indirect lighting (split-sum IBL) ----
    vec3 Fr = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kS_ibl = Fr;
    vec3 kD_ibl = (vec3(1.0) - kS_ibl) * (1.0 - metallic);

    vec3 irradiance = texture(irradianceCube, N).rgb;
    vec3 diffuseIBL = irradiance * litAlbedo;

    vec3 prefilteredColor = textureLod(prefilterCube, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 brdf = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specularIBL = prefilteredColor * (Fr * brdf.x + brdf.y) * u_specularScale;

    vec3 fillAlbedo = max(litAlbedo, vec3(0.42));
    vec3 ambient = (kD_ibl * diffuseIBL + specularIBL) * ao * u_ambientScale * u_iblIntensity;
    ambient += fillAlbedo * u_flatAmbient;
    ambient *= (1.0 + charMask * u_characterAmbientBoost);

    if (u_debugMode == 6) { fragColor = vec4(irradiance, 1.0); return; }
    if (u_debugMode == 9) { fragColor = vec4(diffuseIBL, 1.0); return; }
    if (u_debugMode == 12) { fragColor = vec4(ambient, 1.0); return; }
    if (u_debugMode == 13) { fragColor = vec4(Lo, 1.0); return; }
    if (u_debugMode == 14) { fragColor = vec4(ambient + Lo, 1.0); return; }
    if (u_debugMode == 15) { fragColor = vec4(fillAlbedo * u_flatAmbient, 1.0); return; }
    if (u_debugMode == 16) { fragColor = vec4(litAlbedo, 1.0); return; }

    fragColor = vec4(ambient + Lo, 1.0);
}
