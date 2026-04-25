#pragma once
#include "heightmap.h"
#include <glad/glad.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <cmath>

struct Splatmap {
    int    W = 0, H = 0;
    std::vector<float> data; // W*H*4 floats (RGBA weights per pixel)
    GLuint tex   = 0;
    bool   dirty = false;

    void Resize(int w, int h) {
        W = w; H = h;
        data.assign(w * h * 4, 0.f);
        // Default: all weight on material 0 (red channel)
        for (int i = 0; i < w * h; i++) data[i * 4 + 0] = 1.f;

        if (tex == 0) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        dirty = false;
    }

    void Upload() {
        if (!dirty || tex == 0) return;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_FLOAT, data.data());
        dirty = false;
    }

    void Destroy() {
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
        data.clear(); W = H = 0;
    }

    float* At(int x, int z) { return &data[(z * W + x) * 4]; }

    bool Save(const std::string& path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        uint32_t magic = 0x4D505352; // "RSPM"
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&W), 4);
        f.write(reinterpret_cast<const char*>(&H), 4);
        f.write(reinterpret_cast<const char*>(data.data()), data.size() * 4);
        return true;
    }

    bool Load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        uint32_t magic = 0;
        f.read(reinterpret_cast<char*>(&magic), 4);
        if (magic != 0x4D505352) return false;
        int lw, lh;
        f.read(reinterpret_cast<char*>(&lw), 4);
        f.read(reinterpret_cast<char*>(&lh), 4);
        std::vector<float> tmp(lw * lh * 4);
        f.read(reinterpret_cast<char*>(tmp.data()), tmp.size() * 4);
        if (!f) return false;
        Resize(lw, lh);
        data = std::move(tmp);
        dirty = true;
        return true;
    }
};

// Paint one material layer onto the splatmap at world-space (wx, wz).
// Radius is in world units. matIdx selects which RGBA channel to boost (0-3).
inline void PaintSplatmap(Splatmap& smap, float wx, float wz,
                           float radius, float strength, float dt,
                           int matIdx, float terrainW, float terrainH)
{
    float scaleX = smap.W / terrainW;
    float scaleZ = smap.H / terrainH;
    int   cx     = (int)(wx * scaleX);
    int   cz     = (int)(wz * scaleZ);
    float pixR   = radius * scaleX;
    int   r      = (int)std::ceil(pixR) + 1;

    int x0 = std::max(0, cx - r), x1 = std::min(smap.W - 1, cx + r);
    int z0 = std::max(0, cz - r), z1 = std::min(smap.H - 1, cz + r);

    for (int z = z0; z <= z1; z++) {
        for (int x = x0; x <= x1; x++) {
            float dx   = (float)(x - cx), dz = (float)(z - cz);
            float dist = std::sqrt(dx*dx + dz*dz);
            if (dist > pixR) continue;

            // Gaussian weight
            float sigma = pixR * 0.4f;
            float w = std::exp(-(dist*dist) / (2.f*sigma*sigma)) * strength * dt * 2.5f;
            w = std::min(w, 1.f);

            float* weights = smap.At(x, z);
            float  prev    = weights[matIdx];
            weights[matIdx] = std::min(prev + w * (1.f - prev), 1.f);

            // Redistribute remaining weight across other channels
            float gain   = weights[matIdx] - prev;
            float others = 0.f;
            for (int i = 0; i < 4; i++) if (i != matIdx) others += weights[i];
            if (others > 1e-6f) {
                float ratio = gain / others;
                for (int i = 0; i < 4; i++)
                    if (i != matIdx) weights[i] = std::max(weights[i] - weights[i] * ratio, 0.f);
            }
        }
    }
    smap.dirty = true;
}
