#version 460

layout(location = 0) in vec2 a_xz;   // world-space XZ position

uniform mat4      u_viewProj;
uniform sampler2D u_heightmap;        // R32F, W×H cells
uniform vec2      u_terrainOrigin;    // world-space XZ origin (reused from FS)
uniform vec2      u_terrainSize;      // world-space XZ extents
uniform float     u_cellSize;         // metres per cell — scales finite-difference normal
uniform float     u_lodLevel;         // fractional LOD: 0=full, 1=half, 2=quarter…
uniform float     u_lodBlendRange;    // blend zone per LOD step (0..1), set to 1.0

out vec3 v_worldPos;
out vec3 v_normal;
out vec3 v_tangent;
out vec2 v_uv;

// Sample height at a lower-resolution grid (skipping 2^lodN pixels).
// Snapping to the coarser grid produces the same vertex position as a lower-
// resolution mesh would, so geomorphing eliminates the pop when LOD changes.
float SampleLOD(vec2 baseUV, float lodN) {
    float stepSize = exp2(lodN);
    vec2  hmSize   = vec2(textureSize(u_heightmap, 0));
    vec2  snapped  = round(baseUV * hmSize / stepSize) * stepSize / hmSize;
    return texture(u_heightmap, snapped).r;
}

void main() {
    vec2 uv = (a_xz - u_terrainOrigin) / u_terrainSize;

    // CLOD geomorphing: smoothly interpolate between current and next LOD.
    float lodFloor   = floor(u_lodLevel);
    float lodFrac    = fract(u_lodLevel);
    float hCur       = SampleLOD(uv, lodFloor);
    float hNext      = SampleLOD(uv, lodFloor + 1.0);
    float morphAlpha = smoothstep(1.0 - u_lodBlendRange, 1.0, lodFrac);
    float h          = mix(hCur, hNext, morphAlpha);

    // Finite-difference normals from full-resolution heightmap — continuous
    // across chunk boundaries and stays crisp regardless of LOD level.
    vec2  ts = vec2(1.0) / vec2(textureSize(u_heightmap, 0));
    float hL = texture(u_heightmap, uv - vec2(ts.x, 0.0)).r;
    float hR = texture(u_heightmap, uv + vec2(ts.x, 0.0)).r;
    float hD = texture(u_heightmap, uv - vec2(0.0, ts.y)).r;
    float hU = texture(u_heightmap, uv + vec2(0.0, ts.y)).r;

    v_normal  = normalize(vec3(hL - hR, 2.0 * u_cellSize, hD - hU));
    v_tangent = normalize(vec3(2.0 * u_cellSize, hR - hL, 0.0));
    v_worldPos = vec3(a_xz.x, h, a_xz.y);
    v_uv       = a_xz;
    gl_Position = u_viewProj * vec4(v_worldPos, 1.0);
}
