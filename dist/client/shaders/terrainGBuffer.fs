#version 460

in vec3 v_worldPos;
in vec3 v_normal;
in vec3 v_tangent;
in vec2 v_uv;

uniform sampler2D u_splatmap;

uniform sampler2D u_mat0_albedo;   uniform sampler2D u_mat0_normal;   uniform sampler2D u_mat0_roughness;
uniform sampler2D u_mat1_albedo;   uniform sampler2D u_mat1_normal;   uniform sampler2D u_mat1_roughness;
uniform sampler2D u_mat2_albedo;   uniform sampler2D u_mat2_normal;   uniform sampler2D u_mat2_roughness;
uniform sampler2D u_mat3_albedo;   uniform sampler2D u_mat3_normal;   uniform sampler2D u_mat3_roughness;

uniform float u_tiling;
uniform vec2  u_terrainOrigin;
uniform vec2  u_terrainSize;

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec2 gNormal;
layout(location = 2) out vec4 gRMA;

vec3 triplanar(vec3 p, vec3 n, sampler2D t, float tile) {
    vec3 bw = abs(n);
    bw = max(bw - 0.2, 0.0);
    bw /= dot(bw, vec3(1.0));
    vec3 cx = texture(t, p.yz / tile).rgb;
    vec3 cy = texture(t, p.xz / tile).rgb;
    vec3 cz = texture(t, p.xy / tile).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}
float triplanarR(vec3 p, vec3 n, sampler2D t, float tile) {
    vec3 bw = abs(n);
    bw = max(bw - 0.2, 0.0);
    bw /= dot(bw, vec3(1.0));
    return texture(t, p.yz / tile).r * bw.x
         + texture(t, p.xz / tile).r * bw.y
         + texture(t, p.xy / tile).r * bw.z;
}
vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return (n.z >= 0.0) ? n.xy
           : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                      n.y >= 0.0 ? 1.0 : -1.0);
}

void main() {
    vec2 uvT = (v_worldPos.xz - u_terrainOrigin) / u_terrainSize;
    vec4 w = texture(u_splatmap, uvT);
    float wsum = max(w.r + w.g + w.b + w.a, 1e-4);
    w /= wsum;

    vec3 n  = normalize(v_normal);
    vec3 wp = v_worldPos;

    vec3  alb0 = triplanar (wp, n, u_mat0_albedo,    u_tiling);
    vec3  alb1 = triplanar (wp, n, u_mat1_albedo,    u_tiling);
    vec3  alb2 = triplanar (wp, n, u_mat2_albedo,    u_tiling);
    vec3  alb3 = triplanar (wp, n, u_mat3_albedo,    u_tiling);
    float r0   = triplanarR(wp, n, u_mat0_roughness, u_tiling);
    float r1   = triplanarR(wp, n, u_mat1_roughness, u_tiling);
    float r2   = triplanarR(wp, n, u_mat2_roughness, u_tiling);
    float r3   = triplanarR(wp, n, u_mat3_roughness, u_tiling);

    vec3  alb = alb0*w.r + alb1*w.g + alb2*w.b + alb3*w.a;
    float r   = r0  *w.r + r1  *w.g + r2  *w.b + r3  *w.a;

    gAlbedo = vec4(alb, 1.0);
    gNormal = octEncode(n);
    gRMA    = vec4(r, 0.0, 1.0, 0.0);
}
