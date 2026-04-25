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

uniform float u_tiling;
uniform vec2  u_terrainOrigin;
uniform vec2  u_terrainSize;

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec2 gNormal;
layout(location = 2) out vec4 gRMA;

// ---- Triplanar weights (shared across all material samples) -------------
vec3 triplanarWeights(vec3 n) {
    vec3 bw = abs(n);
    bw = max(bw - 0.2, 0.0);
    bw /= max(dot(bw, vec3(1.0)), 1e-4);
    return bw;
}

vec3 triplanar(vec3 p, vec3 bw, sampler2D t, float tile) {
    vec3 cx = texture(t, p.yz / tile).rgb;
    vec3 cy = texture(t, p.xz / tile).rgb;
    vec3 cz = texture(t, p.xy / tile).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}
float triplanarR(vec3 p, vec3 bw, sampler2D t, float tile) {
    return texture(t, p.yz / tile).r * bw.x
         + texture(t, p.xz / tile).r * bw.y
         + texture(t, p.xy / tile).r * bw.z;
}

// Whiteout-style triplanar normal blend (Ben Golus). Each plane samples a
// tangent-space normal, then the tangent-space delta is added to the vertex
// normal swizzled into that plane's frame. The signed Z handles back-facing
// projections (e.g. the underside of an overhang).
vec3 triplanarNormal(vec3 p, vec3 vn, vec3 bw, sampler2D nmap, float tile) {
    vec3 tx = texture(nmap, p.yz / tile).xyz * 2.0 - 1.0;
    vec3 ty = texture(nmap, p.xz / tile).xyz * 2.0 - 1.0;
    vec3 tz = texture(nmap, p.xy / tile).xyz * 2.0 - 1.0;

    vec3 axisSign = sign(vn);
    tx.z *= axisSign.x;
    ty.z *= axisSign.y;
    tz.z *= axisSign.z;

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

// UE5 LB_HeightBlend — combine splatmap weight with each material's height
// channel so the layer with the most "presence" at a given world point wins.
// `slop` controls how steep the transition is (higher = sharper, more
// natural-looking edges).
vec4 heightBlendWeights(vec4 splat, vec4 heights, float slop) {
    vec4 a = splat * heights;
    float m = max(max(a.r, a.g), max(a.b, a.a));
    vec4 mask = max(a - (m - slop), 0.0);
    float sum = mask.r + mask.g + mask.b + mask.a;
    return mask / max(sum, 1e-4);
}

void main() {
    // Splatmap weight per layer (RGBA → 4 layers).
    vec2 uvT = (v_worldPos.xz - u_terrainOrigin) / u_terrainSize;
    vec4 ws  = texture(u_splatmap, uvT);
    float wsum = max(ws.r + ws.g + ws.b + ws.a, 1e-4);
    ws /= wsum;

    vec3 vn = normalize(v_normal);
    vec3 bw = triplanarWeights(vn);
    vec3 wp = v_worldPos;
    float tile = u_tiling;

    // Per-layer height to drive the height blend. Single-sample triplanar.
    float h0 = triplanarR(wp, bw, u_mat0_height, tile);
    float h1 = triplanarR(wp, bw, u_mat1_height, tile);
    float h2 = triplanarR(wp, bw, u_mat2_height, tile);
    float h3 = triplanarR(wp, bw, u_mat3_height, tile);
    vec4  W  = heightBlendWeights(ws, vec4(h0, h1, h2, h3), 0.2);

    // Albedo blend.
    vec3 alb = triplanar(wp, bw, u_mat0_albedo, tile) * W.r
             + triplanar(wp, bw, u_mat1_albedo, tile) * W.g
             + triplanar(wp, bw, u_mat2_albedo, tile) * W.b
             + triplanar(wp, bw, u_mat3_albedo, tile) * W.a;

    // Normal blend (each layer triplanar-blended, then weighted-summed).
    vec3 n0 = triplanarNormal(wp, vn, bw, u_mat0_normal, tile);
    vec3 n1 = triplanarNormal(wp, vn, bw, u_mat1_normal, tile);
    vec3 n2 = triplanarNormal(wp, vn, bw, u_mat2_normal, tile);
    vec3 n3 = triplanarNormal(wp, vn, bw, u_mat3_normal, tile);
    vec3 nrm = normalize(n0 * W.r + n1 * W.g + n2 * W.b + n3 * W.a);

    // Roughness/AO blends (single-channel triplanar each).
    float rough = triplanarR(wp, bw, u_mat0_roughness, tile) * W.r
                + triplanarR(wp, bw, u_mat1_roughness, tile) * W.g
                + triplanarR(wp, bw, u_mat2_roughness, tile) * W.b
                + triplanarR(wp, bw, u_mat3_roughness, tile) * W.a;

    float ao = triplanarR(wp, bw, u_mat0_ao, tile) * W.r
             + triplanarR(wp, bw, u_mat1_ao, tile) * W.g
             + triplanarR(wp, bw, u_mat2_ao, tile) * W.b
             + triplanarR(wp, bw, u_mat3_ao, tile) * W.a;

    gAlbedo = vec4(alb, 1.0);
    gNormal = octEncode(nrm);
    // gRMA: R=roughness, G=metallic (always 0 for terrain), B=AO.
    gRMA = vec4(clamp(rough, 0.04, 1.0), 0.0, ao, 0.0);
}
