#pragma once
#include "heightmap.h"
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

enum class BrushMode    { Raise, Lower, Smooth, Flatten, Noise };
enum class BrushFalloff { Smooth, Gaussian, Linear, Spherical };

// Brush footprint shape. Only changes the DISTANCE METRIC used to test/weight
// a texel against the brush radius — Circle uses Euclidean distance, Square
// uses Chebyshev (max of the two axis deltas), so the same falloff curves
// above still apply unchanged to whichever shape is selected.
enum class BrushShape { Circle, Square };

// Distance from (dx,dz) to the brush centre, in the same units as `radius`,
// per BrushShape. Circle = Euclidean (a disc); Square = Chebyshev (a box).
inline float ShapeDistance(float dx, float dz, BrushShape shape) {
    if (shape == BrushShape::Square) return std::max(std::abs(dx), std::abs(dz));
    return std::sqrt(dx * dx + dz * dz);
}

// Value noise (hash + smooth/Hermite bilinear interpolation) in [-1,1].
// Factored out of BrushMode::Noise (previously a local lambda + inline block
// in ApplyBrush) so it can also drive the general-purpose "Noise" brush
// parameter used by every brush mode/behaviour (ApplyBrush AND the splatmap
// paint/erase/smooth functions in splatmap.h) — same hash, same
// interpolation, not reimplemented.
inline float ValueNoise2D(float nx, float nz) {
    auto valHash = [](int ix, int iz) -> float {
        unsigned s = (unsigned)(ix * 1619 + iz * 31337);
        s = (s ^ (s >> 16)) * 0x45d9f3bu;
        s ^= s >> 16;
        return ((float)(s & 0xFFFFu) / 65535.f) * 2.f - 1.f;
    };
    int   ix = (int)nx; if (nx < 0.f) ix--;
    int   iz = (int)nz; if (nz < 0.f) iz--;
    float fx = nx - (float)ix, fz = nz - (float)iz;
    float ux = fx * fx * (3.f - 2.f * fx);
    float uz = fz * fz * (3.f - 2.f * fz);
    return glm::mix(glm::mix(valHash(ix,   iz),   valHash(ix+1, iz),   ux),
                    glm::mix(valHash(ix,   iz+1), valHash(ix+1, iz+1), ux), uz);
}

// Sharpens (hardness>0) or leaves unchanged (hardness=0) a falloff weight
// already in [0,1], via a contrast/"gain" remap: monotonic, f(0)=0, f(1)=1,
// f(0.5)=0.5 for every hardness value, so hardness=1 approaches a hard edge
// without an actual branch/step discontinuity. Applied on top of whichever
// BrushFalloff curve is selected instead of trying to interpolate between the
// 4 discrete curve shapes (ambiguous — there's no single natural blend order
// across Smooth/Gaussian/Linear/Spherical). See docs/TECH_DEBT.md "Terrain
// multi-material authoring (Phase 1)" for the brush-system extension notes.
inline float ApplyHardness(float f, float hardness) {
    f = std::clamp(f, 0.f, 1.f);
    hardness = std::clamp(hardness, 0.f, 1.f);
    if (hardness <= 0.f || f <= 0.f || f >= 1.f) return f;
    float k   = glm::mix(1.f, 16.f, hardness);
    float fp  = std::pow(f, k);
    float ifp = std::pow(1.f - f, k);
    return fp / (fp + ifp);
}

// Falloff weight in [0,1] — 1 at centre, 0 at edge. `hardness` (0..1, default
// 0 = unchanged) sharpens the transition edge via ApplyHardness above.
inline float CalcFalloff(float dist, float radius, BrushFalloff type, float hardness = 0.f) {
    float t = 1.f - std::clamp(dist / radius, 0.f, 1.f);
    float f;
    switch (type) {
    case BrushFalloff::Linear:
        f = t;
        break;
    case BrushFalloff::Smooth:        // Hermite — UE default
        f = t * t * (3.f - 2.f * t);
        break;
    case BrushFalloff::Spherical:     // sphere projection
        f = std::sqrt(std::max(0.f, 1.f - (1.f - t) * (1.f - t)));
        break;
    default: {                        // Gaussian
        float sigma = radius * 0.35f;
        f = std::exp(-(dist * dist) / (2.f * sigma * sigma));
        break;
    }
    }
    return ApplyHardness(f, hardness);
}

inline void ApplyBrush(Heightmap& hmap, float wx, float wz,
                       float radius, float strength, float dt,
                       BrushMode mode, float flatten_h = 0.f,
                       BrushFalloff falloff = BrushFalloff::Smooth,
                       BrushShape shape = BrushShape::Circle,
                       float hardness = 0.f, float noiseAmount = 0.f)
{
    float cs = hmap.cell_size;
    int x0 = std::max(0,        (int)((wx - radius) / cs));
    int x1 = std::min(hmap.W-1, (int)((wx + radius) / cs) + 1);
    int z0 = std::max(0,        (int)((wz - radius) / cs));
    int z1 = std::min(hmap.H-1, (int)((wz + radius) / cs) + 1);

    // Brush-relative coords with smooth tiling, used both by BrushMode::Noise
    // (displacement) and by the general noiseAmount modulation below (applied
    // to every mode's weight, not just BrushMode::Noise).
    int   cx_cell    = (int)(wx / cs);
    int   cz_cell    = (int)(wz / cs);
    float noiseScale = std::max((radius / cs) * 0.5f, 1e-4f);

    for (int z = z0; z <= z1; z++) {
        for (int x = x0; x <= x1; x++) {
            float vx   = x * cs;
            float vz   = z * cs;
            float dist = ShapeDistance(vx - wx, vz - wz, shape);
            if (dist > radius) continue;

            float w = CalcFalloff(dist, radius, falloff, hardness);
            if (noiseAmount > 0.f) {
                float nv01 = ValueNoise2D((x - cx_cell) / noiseScale,
                                          (z - cz_cell) / noiseScale) * 0.5f + 0.5f;
                w *= glm::mix(1.f, nv01, noiseAmount);
            }
            w *= strength * dt;
            float h = hmap.Get(x, z);

            switch (mode) {
            case BrushMode::Raise:
                hmap.Set(x, z, h + w * 6.f);
                break;
            case BrushMode::Lower:
                hmap.Set(x, z, h - w * 6.f);
                break;
            case BrushMode::Smooth: {
                float avg = (hmap.Get(x-1,z) + hmap.Get(x+1,z) +
                             hmap.Get(x,z-1) + hmap.Get(x,z+1)) * 0.25f;
                hmap.Set(x, z, h + (avg - h) * std::min(w * 4.f, 1.f));
                break;
            }
            case BrushMode::Flatten:
                hmap.Set(x, z, h + (flatten_h - h) * std::min(w * 4.f, 1.f));
                break;
            case BrushMode::Noise: {
                float noise = ValueNoise2D((float)(x - cx_cell) / noiseScale,
                                           (float)(z - cz_cell) / noiseScale);
                hmap.Set(x, z, h + noise * w * 4.f);
                break;
            }
            }
        }
    }
}
