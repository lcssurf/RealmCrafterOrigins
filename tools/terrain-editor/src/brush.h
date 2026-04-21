#pragma once
#include "heightmap.h"
#include <cmath>
#include <algorithm>

enum class BrushMode { Raise, Lower, Smooth, Flatten };

// Gaussian falloff — smooth center, fades to zero at edge
inline float GaussWeight(float dist, float radius) {
    float sigma = radius * 0.35f;
    return std::exp(-(dist * dist) / (2.f * sigma * sigma));
}

inline void ApplyBrush(Heightmap& hmap, float wx, float wz,
                       float radius, float strength, float dt,
                       BrushMode mode, float flatten_h = 0.f)
{
    float cs = hmap.cell_size;
    int x0 = std::max(0,        (int)((wx - radius) / cs));
    int x1 = std::min(hmap.W-1, (int)((wx + radius) / cs) + 1);
    int z0 = std::max(0,        (int)((wz - radius) / cs));
    int z1 = std::min(hmap.H-1, (int)((wz + radius) / cs) + 1);

    for (int z = z0; z <= z1; z++) {
        for (int x = x0; x <= x1; x++) {
            float vx   = x * cs;
            float vz   = z * cs;
            float dist = std::sqrt((vx-wx)*(vx-wx) + (vz-wz)*(vz-wz));
            if (dist > radius) continue;

            float w = GaussWeight(dist, radius) * strength * dt;
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
            }
        }
    }
}
