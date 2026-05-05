#pragma once
#include "heightmap.h"
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

enum class BrushMode    { Raise, Lower, Smooth, Flatten, Noise };
enum class BrushFalloff { Smooth, Gaussian, Linear, Spherical };

// Falloff weight in [0,1] — 1 at centre, 0 at edge.
inline float CalcFalloff(float dist, float radius, BrushFalloff type) {
    float t = 1.f - std::clamp(dist / radius, 0.f, 1.f);
    switch (type) {
    case BrushFalloff::Linear:
        return t;
    case BrushFalloff::Smooth:        // Hermite — UE default
        return t * t * (3.f - 2.f * t);
    case BrushFalloff::Spherical:     // sphere projection
        return std::sqrt(std::max(0.f, 1.f - (1.f - t) * (1.f - t)));
    default:                          // Gaussian
        float sigma = radius * 0.35f;
        return std::exp(-(dist * dist) / (2.f * sigma * sigma));
    }
}

inline void ApplyBrush(Heightmap& hmap, float wx, float wz,
                       float radius, float strength, float dt,
                       BrushMode mode, float flatten_h = 0.f,
                       BrushFalloff falloff = BrushFalloff::Smooth)
{
    float cs = hmap.cell_size;
    int x0 = std::max(0,        (int)((wx - radius) / cs));
    int x1 = std::min(hmap.W-1, (int)((wx + radius) / cs) + 1);
    int z0 = std::max(0,        (int)((wz - radius) / cs));
    int z1 = std::min(hmap.H-1, (int)((wz + radius) / cs) + 1);

    // Noise mode pre-computation — brush-relative coords with smooth tiling
    int   cx_cell   = (int)(wx / cs);
    int   cz_cell   = (int)(wz / cs);
    float noiseScale = std::max((radius / cs) * 0.5f, 1e-4f);

    auto valHash = [](int ix, int iz) -> float {
        unsigned s = (unsigned)(ix * 1619 + iz * 31337);
        s = (s ^ (s >> 16)) * 0x45d9f3bu;
        s ^= s >> 16;
        return ((float)(s & 0xFFFFu) / 65535.f) * 2.f - 1.f;
    };

    for (int z = z0; z <= z1; z++) {
        for (int x = x0; x <= x1; x++) {
            float vx   = x * cs;
            float vz   = z * cs;
            float dist = std::sqrt((vx-wx)*(vx-wx) + (vz-wz)*(vz-wz));
            if (dist > radius) continue;

            float w = CalcFalloff(dist, radius, falloff) * strength * dt;
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
                float nx = (float)(x - cx_cell) / noiseScale;
                float nz = (float)(z - cz_cell) / noiseScale;
                int   ix = (int)nx; if (nx < 0.f) ix--;
                int   iz = (int)nz; if (nz < 0.f) iz--;
                float fx = nx - (float)ix, fz = nz - (float)iz;
                float ux = fx * fx * (3.f - 2.f * fx);
                float uz = fz * fz * (3.f - 2.f * fz);
                float noise = glm::mix(glm::mix(valHash(ix,   iz),   valHash(ix+1, iz),   ux),
                                       glm::mix(valHash(ix,   iz+1), valHash(ix+1, iz+1), ux), uz);
                hmap.Set(x, z, h + noise * w * 4.f);
                break;
            }
            }
        }
    }
}
