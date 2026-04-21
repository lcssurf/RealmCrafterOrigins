#version 460 core
in  vec3 vWorldPos;
in  vec3 vNormal;
out vec4 FragColor;

uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform vec3 uCamPos;

// PBR material system
uniform int       uUseMaterials;
uniform int       uMatCount;
uniform sampler2D uAlbedo[4];
uniform sampler2D uNormal[4];
uniform sampler2D uRoughness[4];
uniform float     uTiling[4];
uniform sampler2D uSplatmap;
uniform vec2      uTerrainOriginXZ;
uniform vec2      uTerrainSizeXZ;

// ---------------------------------------------------------------------------
// Cook-Torrance BRDF (shared with actor.frag)
// ---------------------------------------------------------------------------
const float PI = 3.14159265359;

float D_GGX(float NdotH, float a2) {
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G_Schlick(float NdotX, float k) {
    return NdotX / (NdotX * (1.0 - k) + k);
}

float G_Smith(float NdotL, float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return G_Schlick(NdotL, k) * G_Schlick(NdotV, k);
}

vec3 F_Schlick(float VdotH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

// Terrain is always dielectric (metallic=0), so diffuse albedo / PI + specular
vec3 TerrainBRDF(vec3 albedo, float roughness,
                 vec3 N, vec3 V, vec3 L, vec3 lightColor) {
    vec3  H     = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3  F0 = vec3(0.04); // dielectric
    float a2 = pow(roughness * roughness, 2.0);

    float D = D_GGX(NdotH, a2);
    float G = G_Smith(NdotL, NdotV, roughness);
    vec3  F = F_Schlick(VdotH, F0);

    vec3 specular = (D * G * F) / max(4.0 * NdotL * NdotV, 0.001);
    vec3 kD       = (1.0 - F); // no metallic, so kD is full
    vec3 diffuse  = kD * albedo / PI;

    return (diffuse + specular) * lightColor * NdotL;
}

// ---------------------------------------------------------------------------
// Procedural height color (fallback when no materials)
// ---------------------------------------------------------------------------
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
    else               return mix(rock,        snow,      clamp((h - 10.0) / 5.0, 0.0, 1.0));
}

// ---------------------------------------------------------------------------
// Triplanar helpers
// ---------------------------------------------------------------------------
vec3 TriBlend(vec3 n) {
    vec3 b = max(abs(n) - 0.2, 0.0);
    b = b * b;
    return b / dot(b, vec3(1.0));
}

vec3 TriAlbedo(sampler2D tex, vec3 pos, float tiling, vec3 blend) {
    vec3 x = texture(tex, pos.zy * tiling).rgb;
    vec3 y = texture(tex, pos.xz * tiling).rgb;
    vec3 z = texture(tex, pos.xy * tiling).rgb;
    return x * blend.x + y * blend.y + z * blend.z;
}

vec3 TriNormal(sampler2D tex, vec3 pos, float tiling, vec3 blend, vec3 N) {
    vec3 tnx = texture(tex, pos.zy * tiling).rgb * 2.0 - 1.0;
    vec3 tny = texture(tex, pos.xz * tiling).rgb * 2.0 - 1.0;
    vec3 tnz = texture(tex, pos.xy * tiling).rgb * 2.0 - 1.0;
    vec3 nx = vec3(tnx.z, tnx.y, tnx.x) * sign(N.x);
    vec3 ny = vec3(tny.x, tny.z, tny.y) * sign(N.y);
    vec3 nz = vec3(tnz.x, tnz.y, tnz.z) * sign(N.z);
    return normalize(nx * blend.x + ny * blend.y + nz * blend.z + N);
}

float TriRoughness(sampler2D tex, vec3 pos, float tiling, vec3 blend) {
    float rx = texture(tex, pos.zy * tiling).g;
    float ry = texture(tex, pos.xz * tiling).g;
    float rz = texture(tex, pos.xy * tiling).g;
    return rx * blend.x + ry * blend.y + rz * blend.z;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);
    vec3 L = normalize(uSunDir);

    vec3  color;

    if (uUseMaterials == 1 && uMatCount > 0) {
        vec3  blend   = TriBlend(N);
        vec2  splatUV = clamp((vWorldPos.xz - uTerrainOriginXZ) / uTerrainSizeXZ, 0.0, 1.0);
        vec4  weights = texture(uSplatmap, splatUV);
        float wsum    = dot(weights, vec4(1.0));
        if (wsum > 0.001) weights /= wsum; else weights = vec4(1, 0, 0, 0);

        // --- Height-based blending (UE5 LB_HeightBlend formula) ---
        // Convert splatmap weight to [-1,1], add luminance height hint,
        // clamp, then renormalize. Layers with low splatmap weight can still
        // win if their albedo is bright enough — gives sharp, natural borders.
        vec3 albedos[4];
        vec4 w_hb = vec4(0.0);
        for (int i = 0; i < 4; i++) {
            if (i >= uMatCount) break;
            albedos[i] = TriAlbedo(uAlbedo[i], vWorldPos, uTiling[i], blend);
            float h = dot(albedos[i], vec3(0.299, 0.587, 0.114));
            w_hb[i] = clamp((weights[i] * 2.0 - 1.0) + h, 0.0001, 1.0);
        }
        float wsum2 = dot(w_hb, vec4(1.0));
        if (wsum2 > 0.001) w_hb /= wsum2; else w_hb = vec4(1, 0, 0, 0);

        vec3  albedo    = vec3(0.0);
        vec3  normal    = vec3(0.0);
        float roughness = 0.0;

        for (int i = 0; i < 4; i++) {
            if (i >= uMatCount) break;
            float w = w_hb[i];
            if (w < 0.001) continue;
            float t = uTiling[i];
            albedo    += albedos[i] * w;
            normal    += TriNormal   (uNormal[i],    vWorldPos, t, blend, N) * w;
            roughness += TriRoughness(uRoughness[i], vWorldPos, t, blend) * w;
        }
        normal    = normalize(normal);
        roughness = clamp(roughness, 0.04, 1.0);

        color = TerrainBRDF(albedo, roughness, normal, V, L, uSunColor);
        color += albedo * uAmbient; // ambient (no AO on terrain for now)

        // Tone map
        color = color / (color + vec3(1.0));
    } else {
        // Procedural fallback — no tone mapping (low dynamic range anyway)
        vec3  base = HeightColor(vWorldPos.y);
        float diff = max(dot(N, L), 0.0);
        vec3  R    = reflect(-L, N);
        float spec = pow(max(dot(normalize(uCamPos - vWorldPos), R), 0.0), 32.0)
                     * ((vWorldPos.y < 0.0) ? 0.3 : 0.02);
        color = base * (uAmbient + uSunColor * diff) + uSunColor * spec;
    }

    float fog = clamp((length(uCamPos - vWorldPos) - 150.0) / 200.0, 0.0, 1.0);
    FragColor = vec4(mix(color, vec3(0.60, 0.75, 0.90), fog), 1.0);
}
