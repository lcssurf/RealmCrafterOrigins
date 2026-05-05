#version 460

in vec3 v_worldPos;
in vec3 v_normal;
in vec3 v_tangent;
in vec2 v_uv;

uniform sampler2D u_splatmap;

uniform sampler2D u_mat0_albedo;   uniform sampler2D u_mat0_normal;
uniform sampler2D u_mat0_roughness; uniform sampler2D u_mat0_ao; uniform sampler2D u_mat0_height;
uniform sampler2D u_mat1_albedo;   uniform sampler2D u_mat1_normal;
uniform sampler2D u_mat1_roughness; uniform sampler2D u_mat1_ao; uniform sampler2D u_mat1_height;
uniform sampler2D u_mat2_albedo;   uniform sampler2D u_mat2_normal;
uniform sampler2D u_mat2_roughness; uniform sampler2D u_mat2_ao; uniform sampler2D u_mat2_height;
uniform sampler2D u_mat3_albedo;   uniform sampler2D u_mat3_normal;
uniform sampler2D u_mat3_roughness; uniform sampler2D u_mat3_ao; uniform sampler2D u_mat3_height;

uniform vec4  u_tilings;         // xyzw = tiling per layer (0-3)
uniform vec2  u_terrainOrigin;
uniform vec2  u_terrainSize;
uniform vec3  u_cameraPos;          // world-space camera position
uniform sampler2D u_macroVariation; // single-channel, covers full terrain
uniform float u_macroStrength;           // 0 = off, 0.3 = subtle, 1 = full
uniform float u_mat0_normal_strength;    // per-material normal map intensity
uniform float u_mat1_normal_strength;
uniform float u_mat2_normal_strength;
uniform float u_mat3_normal_strength;

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec2 gNormal;
layout(location = 2) out vec4 gRMA;

// ---- Pseudo-random 2D hash (Quilez) ----------------------------------------
vec2 hash22(vec2 p) {
    p  = fract(p * vec2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return fract((p.xx + p.yy) * p.yx);
}

// ---- Stochastic tile setup (Deliot & Cohen-Steiner 2019) -------------------
// Partitions the 2D UV plane into a triangular grid. Every point belongs to one
// of two triangles in the unit square; the 3 triangle vertices each receive a
// deterministic random phase offset (o0..o2). Weights W are Hermite-smoothed
// barycentric coords — removes visible seams at cell boundaries.
void stochCoords(vec2 uv, out vec3 W, out vec2 o0, out vec2 o1, out vec2 o2) {
    vec2  f  = fract(uv);
    vec2  vi = floor(uv);
    vec2  i0, i1, i2;
    float w0, w1, w2;
    if (f.x + f.y < 1.0) {          // lower-left triangle
        i0 = vi;              w0 = 1.0 - f.x - f.y;
        i1 = vi + vec2(1, 0); w1 = f.x;
        i2 = vi + vec2(0, 1); w2 = f.y;
    } else {                          // upper-right triangle
        i0 = vi + vec2(1, 1); w0 = f.x + f.y - 1.0;
        i1 = vi + vec2(0, 1); w1 = 1.0 - f.x;
        i2 = vi + vec2(1, 0); w2 = 1.0 - f.y;
    }
    W  = vec3(w0, w1, w2);
    W  = W * W * (3.0 - 2.0 * W);   // Hermite smoothstep — no seam artifacts
    W /= (W.x + W.y + W.z);          // renormalise after smoothing
    o0 = hash22(i0);
    o1 = hash22(i1);
    o2 = hash22(i2);
}

// Histogram-preserving RGB blend: work in log-space so the blended texture
// preserves the original's value histogram (avoids the desaturated wash-out
// that plain linear blending of 3 offset samples produces).
// textureGrad with the NON-offset UV gradient is critical: it forces the GPU
// to pick the mip level for the intended tile scale, not for the random offsets
// (which would cause too-fine mip selection and shimmering at hash boundaries).
vec3 sampleStoch(sampler2D t, vec2 uv) {
    vec3 W; vec2 o0, o1, o2;
    stochCoords(uv, W, o0, o1, o2);
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    const float eps = 1e-3;
    vec3 c0 = textureGrad(t, uv + o0, dx, dy).rgb;
    vec3 c1 = textureGrad(t, uv + o1, dx, dy).rgb;
    vec3 c2 = textureGrad(t, uv + o2, dx, dy).rgb;
    return max(exp(W.x*log(c0+eps) + W.y*log(c1+eps) + W.z*log(c2+eps)) - eps, vec3(0.0));
}

// ---- Triplanar weights (shared across all material samples) ----------------
vec3 triplanarWeights(vec3 n) {
    vec3 bw = abs(n);
    bw = max(bw - 0.2, 0.0);
    bw /= max(dot(bw, vec3(1.0)), 1e-4);
    return bw;
}

// Albedo: full stochastic triplanar (histogram-preserving, no visible repetition).
vec3 triplanar(vec3 p, vec3 bw, sampler2D t, float tile) {
    return sampleStoch(t, p.yz / tile) * bw.x
         + sampleStoch(t, p.xz / tile) * bw.y
         + sampleStoch(t, p.xy / tile) * bw.z;
}

// Roughness/AO/height: plain triplanar — tiling patterns in these channels are
// far less perceptible than in albedo; stochastic is not worth the extra cost.
float triplanarR(vec3 p, vec3 bw, sampler2D t, float tile) {
    return texture(t, p.yz / tile).r * bw.x
         + texture(t, p.xz / tile).r * bw.y
         + texture(t, p.xy / tile).r * bw.z;
}
float triplanarG(vec3 p, vec3 bw, sampler2D t, float tile) {
    return texture(t, p.yz / tile).g * bw.x
         + texture(t, p.xz / tile).g * bw.y
         + texture(t, p.xy / tile).g * bw.z;
}

// Whiteout-style triplanar normal blend (Ben Golus) with stochastic sampling.
// Each projection plane blends 3 random-phase samples in tangent space before
// the whiteout construction — eliminates repeating normal patterns.
// DirectX normal maps (green channel inverted): tx.y = -tx.y compensates.
// Multiply XY by strength before whiteout to recover detail lost on top-facing
// surfaces where abs(vn.y)≈1 naturally halves the perturbation.
vec3 triplanarNormal(vec3 p, vec3 vn, vec3 bw, sampler2D nmap, float tile, float strength) {
    vec3 W; vec2 o0, o1, o2, dx, dy;

    // X-plane (YZ projection)
    vec2 uvYZ = p.yz / tile;
    dx = dFdx(uvYZ); dy = dFdy(uvYZ);
    stochCoords(uvYZ, W, o0, o1, o2);
    vec3 tx = (textureGrad(nmap, uvYZ+o0, dx, dy).xyz * 2.0 - 1.0) * W.x
            + (textureGrad(nmap, uvYZ+o1, dx, dy).xyz * 2.0 - 1.0) * W.y
            + (textureGrad(nmap, uvYZ+o2, dx, dy).xyz * 2.0 - 1.0) * W.z;
    tx.y = -tx.y; tx.xy *= strength;

    // Y-plane (XZ projection)
    vec2 uvXZ = p.xz / tile;
    dx = dFdx(uvXZ); dy = dFdy(uvXZ);
    stochCoords(uvXZ, W, o0, o1, o2);
    vec3 ty = (textureGrad(nmap, uvXZ+o0, dx, dy).xyz * 2.0 - 1.0) * W.x
            + (textureGrad(nmap, uvXZ+o1, dx, dy).xyz * 2.0 - 1.0) * W.y
            + (textureGrad(nmap, uvXZ+o2, dx, dy).xyz * 2.0 - 1.0) * W.z;
    ty.y = -ty.y; ty.xy *= strength;

    // Z-plane (XY projection)
    vec2 uvXY = p.xy / tile;
    dx = dFdx(uvXY); dy = dFdy(uvXY);
    stochCoords(uvXY, W, o0, o1, o2);
    vec3 tz = (textureGrad(nmap, uvXY+o0, dx, dy).xyz * 2.0 - 1.0) * W.x
            + (textureGrad(nmap, uvXY+o1, dx, dy).xyz * 2.0 - 1.0) * W.y
            + (textureGrad(nmap, uvXY+o2, dx, dy).xyz * 2.0 - 1.0) * W.z;
    tz.y = -tz.y; tz.xy *= strength;

    vec3 axisSign = sign(vn);
    tx.z *= axisSign.x; ty.z *= axisSign.y; tz.z *= axisSign.z;

    vec3 wx = vec3(tx.xy + vn.zy, abs(vn.x) + tx.z).zyx;
    vec3 wy = vec3(ty.xy + vn.xz, abs(vn.y) + ty.z).xzy;
    vec3 wz = vec3(tz.xy + vn.xy, abs(vn.z) + tz.z);

    return normalize(wx * bw.x + wy * bw.y + wz * bw.z);
}

vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return (n.z >= 0.0) ? n.xy
           : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                      n.y >= 0.0 ? 1.0 : -1.0);
}

// UE5 LB_HeightBlend
vec4 heightBlendWeights(vec4 splat, vec4 heights, float slop) {
    vec4 a = splat * heights;
    float m = max(max(a.r, a.g), max(a.b, a.a));
    vec4 mask = max(a - (m - slop), 0.0);
    float sum = mask.r + mask.g + mask.b + mask.a;
    return mask / max(sum, 1e-4);
}

void main() {
    vec2 uvT = (v_worldPos.xz - u_terrainOrigin) / u_terrainSize;
    vec4 ws  = texture(u_splatmap, uvT);
    float wsum = max(ws.r + ws.g + ws.b + ws.a, 1e-4);
    ws /= wsum;

    vec3 vn = normalize(v_normal);
    vec3 bw = triplanarWeights(vn);
    vec3 wp = v_worldPos;

    // Stochastic tiling eliminates periodic repetition at all distances;
    // rely on mip-mapping for LOD detail. No UV-scale distance fade needed.
    vec4 eff = u_tilings;

    // Per-layer height (plain triplanar — blend-weight errors at tile
    // boundaries are invisible after height-blending).
    float h0 = triplanarR(wp, bw, u_mat0_height, eff.x);
    float h1 = triplanarR(wp, bw, u_mat1_height, eff.y);
    float h2 = triplanarR(wp, bw, u_mat2_height, eff.z);
    float h3 = triplanarR(wp, bw, u_mat3_height, eff.w);
    vec4  W  = heightBlendWeights(ws, vec4(h0, h1, h2, h3), 0.2);

    // Albedo — stochastic triplanar (histogram-preserving, no visible repetition).
    vec3 alb = triplanar(wp, bw, u_mat0_albedo, eff.x) * W.r
             + triplanar(wp, bw, u_mat1_albedo, eff.y) * W.g
             + triplanar(wp, bw, u_mat2_albedo, eff.z) * W.b
             + triplanar(wp, bw, u_mat3_albedo, eff.w) * W.a;

    // Normals — stochastic triplanar (linear blend in tangent space).
    vec3 n0 = triplanarNormal(wp, vn, bw, u_mat0_normal, eff.x, u_mat0_normal_strength);
    vec3 n1 = triplanarNormal(wp, vn, bw, u_mat1_normal, eff.y, u_mat1_normal_strength);
    vec3 n2 = triplanarNormal(wp, vn, bw, u_mat2_normal, eff.z, u_mat2_normal_strength);
    vec3 n3 = triplanarNormal(wp, vn, bw, u_mat3_normal, eff.w, u_mat3_normal_strength);
    vec3 nrm = normalize(n0 * W.r + n1 * W.g + n2 * W.b + n3 * W.a);

    // Roughness — G channel of ORM (plain triplanar).
    float rough = triplanarG(wp, bw, u_mat0_roughness, eff.x) * W.r
                + triplanarG(wp, bw, u_mat1_roughness, eff.y) * W.g
                + triplanarG(wp, bw, u_mat2_roughness, eff.z) * W.b
                + triplanarG(wp, bw, u_mat3_roughness, eff.w) * W.a;

    // AO — R channel of ORM (plain triplanar).
    float ao = triplanarR(wp, bw, u_mat0_ao, eff.x) * W.r
             + triplanarR(wp, bw, u_mat1_ao, eff.y) * W.g
             + triplanarR(wp, bw, u_mat2_ao, eff.z) * W.b
             + triplanarR(wp, bw, u_mat3_ao, eff.w) * W.a;

    // Macro variation: overlay blend breaks large-scale colour monotony.
    // Guard: when u_macroVariation is unassigned (GL default texture = 0),
    // sampling returns black (macro=0) — the overlay formula would darken
    // the terrain. Skip entirely when strength is 0 or no texture is set.
    if (u_macroStrength > 0.0) {
        float macro = texture(u_macroVariation, uvT).r;
        // Neutral at 0.5: both branches evaluate to `alb` when macro == 0.5.
        // Below 0.5 → darkens; above 0.5 → lightens.
        vec3 overlaid = mix(2.0 * alb * macro,
                            1.0 - 2.0 * (1.0 - alb) * (1.0 - macro),
                            step(0.5, macro));
        alb = mix(alb, overlaid, u_macroStrength);
    }

    gAlbedo = vec4(alb, 1.0);
    gNormal = octEncode(nrm);
    // gRMA: R=roughness, G=metallic (0 for terrain), B=AO.
    gRMA    = vec4(clamp(rough, 0.04, 1.0), 0.0, ao, 0.0);
}
