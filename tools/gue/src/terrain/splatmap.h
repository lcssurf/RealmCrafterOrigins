#pragma once
#include "heightmap.h"
#include "brush.h"
#include <glad/glad.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <cmath>

struct Splatmap {
    int    W = 0, H = 0;
    std::vector<uint8_t> data; // W*H*4 bytes (RGBA weights, 0-255 per channel)
    GLuint tex   = 0;
    bool   dirty = false;

    // Accessors — convert between [0,1] floats and uint8 storage
    float   GetWeight(int x, int z, int ch) const {
        return data[(z * W + x) * 4 + ch] / 255.f;
    }
    void    SetWeight(int x, int z, int ch, float v) {
        data[(z * W + x) * 4 + ch] =
            static_cast<uint8_t>(std::clamp(v * 255.f + 0.5f, 0.f, 255.f));
    }

    void Resize(int w, int h) {
        W = w; H = h;
        data.assign(w * h * 4, 0u);
        // Default: all weight on material 0 (red channel)
        for (int i = 0; i < w * h; i++) data[i * 4 + 0] = 255u;

        if (tex == 0) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        dirty = false;
    }

    void Upload() {
        if (!dirty || tex == 0) return;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
        dirty = false;
    }

    void Destroy() {
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
        data.clear(); W = H = 0;
    }

    bool Save(const std::string& path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        uint32_t magic = 0x32505352; // "RSP2"
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&W), 4);
        f.write(reinterpret_cast<const char*>(&H), 4);
        f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        return true;
    }

    bool Load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        uint32_t magic = 0;
        f.read(reinterpret_cast<char*>(&magic), 4);
        int lw, lh;
        f.read(reinterpret_cast<char*>(&lw), 4);
        f.read(reinterpret_cast<char*>(&lh), 4);
        if (!f) return false;

        if (magic == 0x32505352) {
            // RSP2 — native uint8 format
            std::vector<uint8_t> tmp((size_t)lw * lh * 4);
            f.read(reinterpret_cast<char*>(tmp.data()), (std::streamsize)tmp.size());
            if (!f) return false;
            Resize(lw, lh);
            data = std::move(tmp);
        } else if (magic == 0x4D505352) {
            // RSPM — legacy float format; auto-convert on load
            std::vector<float> ftmp((size_t)lw * lh * 4);
            f.read(reinterpret_cast<char*>(ftmp.data()), (std::streamsize)(ftmp.size() * 4));
            if (!f) return false;
            Resize(lw, lh);
            for (size_t i = 0; i < ftmp.size(); i++)
                data[i] = static_cast<uint8_t>(std::clamp(ftmp[i] * 255.f + 0.5f, 0.f, 255.f));
        } else {
            return false;
        }

        dirty = true;
        return true;
    }
};

// Paint one material layer onto the splatmap at world-space (wx, wz).
// Radius is in world units. matIdx selects which RGBA channel to boost (0-3).
inline void PaintSplatmap(Splatmap& smap, float wx, float wz,
                           float radius, float strength, float dt,
                           int matIdx, float terrainW, float terrainH,
                           BrushFalloff falloff = BrushFalloff::Smooth)
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

            float w = CalcFalloff(dist, pixR, falloff) * strength * dt * 2.5f;
            w = std::min(w, 1.f);

            // Read all channels as float first to avoid per-channel uint8 rounding
            // during the redistribute step.
            float ch[4];
            for (int i = 0; i < 4; i++) ch[i] = smap.GetWeight(x, z, i);

            float prev = ch[matIdx];
            float newV = std::min(prev + w * (1.f - prev), 1.f);
            ch[matIdx] = newV;

            // Redistribute lost weight proportionally from the other channels
            float gain   = newV - prev;
            float others = 0.f;
            for (int i = 0; i < 4; i++) if (i != matIdx) others += ch[i];
            if (others > 1e-6f) {
                float ratio = gain / others;
                for (int i = 0; i < 4; i++)
                    if (i != matIdx) ch[i] = std::max(ch[i] - ch[i] * ratio, 0.f);
            }

            // Write all channels at once — single rounding pass per pixel
            for (int i = 0; i < 4; i++) smap.SetWeight(x, z, i, ch[i]);
        }
    }
    smap.dirty = true;
}
