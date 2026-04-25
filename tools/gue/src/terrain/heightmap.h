#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <glm/glm.hpp>

class Heightmap {
public:
    int   W = 0, H = 0;
    float cell_size = 2.f;
    std::vector<float> heights;

    void Resize(int w, int h, float cs = 2.f) {
        W = w; H = h; cell_size = cs;
        heights.assign(w * h, 0.f);
    }

    float Get(int x, int z) const {
        if (x < 0 || x >= W || z < 0 || z >= H) return 0.f;
        return heights[z * W + x];
    }

    void Set(int x, int z, float v) {
        if (x < 0 || x >= W || z < 0 || z >= H) return;
        heights[z * W + x] = v;
    }

    // Bilinear sample from world-space XZ coords
    float SampleWorld(float wx, float wz) const {
        float fx = wx / cell_size;
        float fz = wz / cell_size;
        int   ix = (int)fx, iz = (int)fz;
        float tx = fx - ix,  tz = fz - iz;
        ix = std::clamp(ix, 0, W - 2);
        iz = std::clamp(iz, 0, H - 2);
        float h00 = Get(ix,     iz);
        float h10 = Get(ix + 1, iz);
        float h01 = Get(ix,     iz + 1);
        float h11 = Get(ix + 1, iz + 1);
        return glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
    }

    float WorldW() const { return W * cell_size; }
    float WorldH() const { return H * cell_size; }

    bool Save(const std::string& path) const;
    bool Load(const std::string& path);
};
