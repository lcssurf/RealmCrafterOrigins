#pragma once
#include "brush.h"
#include <glad/glad.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

// TEMP DEBUG (investigation only — remove before commit): logs every GL
// error raised since the last check, tagged with the exact call site. Used
// to bisect the "+ Add Material" transition step by step — see
// docs/TECH_DEBT.md "Terrain multi-material authoring (Phase 1)".
inline void TerrainCheckGL_(const char* where) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR)
        std::fprintf(stderr, "[gl-err] %s: GL error 0x%04x\n", where, err);
}

// One RGBA8 texture holding blend weights for up to 4 materials (one per
// channel). Splatmap below stacks any number of these so authoring is no
// longer capped at 4 materials — material index i lives in layer i/4,
// channel i%4.
struct SplatLayer {
    int    W = 0, H = 0;
    std::vector<uint8_t> data; // W*H*4 bytes (RGBA weights, 0-255 per channel)
    GLuint tex   = 0;
    bool   dirty = false;

    float GetWeight(int x, int z, int ch) const {
        return data[(z * W + x) * 4 + ch] / 255.f;
    }
    void SetWeight(int x, int z, int ch, float v) {
        data[(z * W + x) * 4 + ch] =
            static_cast<uint8_t>(std::clamp(v * 255.f + 0.5f, 0.f, 255.f));
    }

    void Resize(int w, int h, bool defaultFirstChannel) {
        W = w; H = h;
        data.assign((size_t)w * h * 4, 0u);
        if (defaultFirstChannel)
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
};

// Stack of SplatLayers — supports an unbounded number of materials (Phase 1:
// authoring is no longer limited to 4). Material index i resolves to layer
// i/4, channel i%4. Also maintains a companion GL_TEXTURE_2D_ARRAY (one array
// layer per SplatLayer) so the shader can sample every group through a
// single sampler2DArray unit, independent of how many groups exist — see
// docs/TECH_DEBT.md "Terrain multi-material authoring (Phase 1)".
struct Splatmap {
    std::vector<SplatLayer> layers;
    int  W = 0, H = 0;
    bool dirty = false; // true after any paint/undo op; drives Upload()

    GLuint arrayTex = 0; // GL_TEXTURE_2D_ARRAY, layers.size() layers

    int NumGroups() const { return (int)layers.size(); }
    int NumMaterialSlots() const { return (int)layers.size() * 4; }

    // Wipes and reallocates the whole stack: ceil(numMaterials/4) layers,
    // each W×H. Material 0 defaults to full weight (matches legacy behaviour
    // of a brand-new terrain being fully painted with the first material).
    void Resize(int w, int h, int numMaterials = 4) {
        W = w; H = h;
        int numLayers = std::max(1, (numMaterials + 3) / 4);
        layers.assign(numLayers, SplatLayer{});
        for (int i = 0; i < numLayers; ++i)
            layers[i].Resize(w, h, /*defaultFirstChannel=*/ i == 0);
        RebuildArrayStorage();
        dirty = true;
    }

    // Grows the stack to cover `numMaterials` without touching existing
    // pixel data — used when the dev registers another material while
    // editing an already-painted area.
    void EnsureMaterialCount(int numMaterials) {
        int numLayers = std::max(1, (numMaterials + 3) / 4);
        if (numLayers <= (int)layers.size()) return;
        int oldCount = (int)layers.size();
        layers.resize(numLayers);
        for (int i = oldCount; i < numLayers; ++i)
            layers[i].Resize(W, H, false);
        RebuildArrayStorage();
        dirty = true;
    }

    // On-demand debug dump (F11 in the GUE, see ZonesTab) — appends a
    // multi-point weight report for every layer to an already-open file.
    // Reads straight from the GPU array texture (what the shader actually
    // samples), not the CPU-side buffer, so it reflects the true post-upload
    // state. 8 points spread across the map (4 corners + center + 3
    // off-center) catch corruption localised to only part of the terrain,
    // which a single fixed texel check would miss.
    void WriteDebugReport(FILE* f) const {
        std::fprintf(f, "-- Splatmap: W=%d H=%d numLayers=%d arrayTex=%u --\n", W, H, (int)layers.size(), arrayTex);
        if (layers.empty() || W == 0 || H == 0 || arrayTex == 0) {
            std::fprintf(f, "  (no layers / no array texture yet)\n");
            return;
        }
        struct Pt { int x, z; const char* name; };
        Pt pts[8] = {
            {(int)(W * 0.1f), (int)(H * 0.1f), "10%,10%  (corner)"},
            {W - 1,           0,               "corner TR"},
            {0,               H - 1,           "corner BL"},
            {(int)(W * 0.9f), (int)(H * 0.9f), "90%,90%  (corner)"},
            {W / 2,           H / 2,           "50%,50%  (center)"},
            {W / 4,           H / 4,           "25%,25%"},
            {3 * W / 4,       H / 4,           "75%,25%"},
            {W / 3,           2 * H / 3,       "33%,66%"},
        };
        for (int layer = 0; layer < (int)layers.size(); ++layer) {
            std::fprintf(f, "  layer %d (materials %d-%d):\n", layer, layer * 4, layer * 4 + 3);
            for (const auto& p : pts) {
                uint8_t px[4] = {0, 0, 0, 0};
                glGetTextureSubImage(arrayTex, 0, p.x, p.z, layer, 1, 1, 1,
                                      GL_RGBA, GL_UNSIGNED_BYTE, sizeof(px), px);
                std::fprintf(f, "    (%4d,%4d) %-20s R=%3u G=%3u B=%3u A=%3u\n",
                             p.x, p.z, p.name, px[0], px[1], px[2], px[3]);
            }
        }
    }

    float GetWeight(int x, int z, int matIdx) const {
        int l = matIdx / 4, ch = matIdx % 4;
        return (l >= 0 && l < (int)layers.size()) ? layers[l].GetWeight(x, z, ch) : 0.f;
    }
    void SetWeight(int x, int z, int matIdx, float v) {
        int l = matIdx / 4, ch = matIdx % 4;
        if (l >= 0 && l < (int)layers.size()) { layers[l].SetWeight(x, z, ch, v); dirty = true; }
    }

    void Upload() {
        if (!dirty) return;
        for (auto& l : layers) { l.dirty = true; l.Upload(); }
        UploadArray();
        dirty = false;
    }

    void Destroy() {
        for (auto& l : layers) l.Destroy();
        layers.clear();
        if (arrayTex) { glDeleteTextures(1, &arrayTex); arrayTex = 0; }
        W = H = 0;
    }

    // --- Persistence ---
    // Native format "RSPN" (multi-layer): magic | W | H | numLayers | layer0
    // data (W*H*4 bytes) | layer1 data | ... Falls back to reading the
    // legacy single-layer RSP2 (uint8) / RSPM (float) formats into layer 0.
    bool Save(const std::string& path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        uint32_t magic = 0x4E505352; // "RSPN"
        int numLayers = (int)layers.size();
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&W), 4);
        f.write(reinterpret_cast<const char*>(&H), 4);
        f.write(reinterpret_cast<const char*>(&numLayers), 4);
        for (const auto& l : layers)
            f.write(reinterpret_cast<const char*>(l.data.data()), (std::streamsize)l.data.size());
        return true;
    }

    bool Load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        uint32_t magic = 0;
        f.read(reinterpret_cast<char*>(&magic), 4);
        int lw = 0, lh = 0;
        f.read(reinterpret_cast<char*>(&lw), 4);
        f.read(reinterpret_cast<char*>(&lh), 4);
        if (!f) return false;

        if (magic == 0x4E505352) {
            // RSPN — native multi-layer format
            int numLayers = 1;
            f.read(reinterpret_cast<char*>(&numLayers), 4);
            if (!f || numLayers <= 0) return false;
            Resize(lw, lh, numLayers * 4);
            for (auto& l : layers) {
                f.read(reinterpret_cast<char*>(l.data.data()), (std::streamsize)l.data.size());
                if (!f) return false;
                l.dirty = true;
            }
        } else if (magic == 0x32505352) {
            // RSP2 — legacy single-layer uint8 format
            std::vector<uint8_t> tmp((size_t)lw * lh * 4);
            f.read(reinterpret_cast<char*>(tmp.data()), (std::streamsize)tmp.size());
            if (!f) return false;
            Resize(lw, lh, 4);
            layers[0].data  = std::move(tmp);
            layers[0].dirty = true;
        } else if (magic == 0x4D505352) {
            // RSPM — legacy float format; auto-convert on load
            std::vector<float> ftmp((size_t)lw * lh * 4);
            f.read(reinterpret_cast<char*>(ftmp.data()), (std::streamsize)(ftmp.size() * 4));
            if (!f) return false;
            Resize(lw, lh, 4);
            for (size_t i = 0; i < ftmp.size(); i++)
                layers[0].data[i] = static_cast<uint8_t>(std::clamp(ftmp[i] * 255.f + 0.5f, 0.f, 255.f));
            layers[0].dirty = true;
        } else {
            return false;
        }

        dirty = true;
        Upload();
        return true;
    }

    // --- Undo/redo snapshot support (flattened across all layers) ---
    std::vector<uint8_t> AllData() const {
        std::vector<uint8_t> out;
        out.reserve(layers.size() * (size_t)W * H * 4);
        for (const auto& l : layers) out.insert(out.end(), l.data.begin(), l.data.end());
        return out;
    }
    void SetAllData(const std::vector<uint8_t>& flat) {
        size_t per = (size_t)W * H * 4;
        for (size_t i = 0; i < layers.size(); ++i) {
            size_t off = i * per;
            if (off + per > flat.size()) break;
            std::copy(flat.begin() + off, flat.begin() + off + per, layers[i].data.begin());
        }
        dirty = true;
    }

private:
    void RebuildArrayStorage() {
        if (arrayTex) { glDeleteTextures(1, &arrayTex); arrayTex = 0; }
        TerrainCheckGL_("Splatmap::RebuildArrayStorage after glDeleteTextures");
        if (layers.empty() || W == 0 || H == 0) return;
        glGenTextures(1, &arrayTex);
        TerrainCheckGL_("Splatmap::RebuildArrayStorage after glGenTextures");
        glBindTexture(GL_TEXTURE_2D_ARRAY, arrayTex);
        TerrainCheckGL_("Splatmap::RebuildArrayStorage after glBindTexture");
        // Allocated with nullptr (GPU storage starts undefined/garbage —
        // NOT zeroed by the driver). RebuildArrayStorage() runs on every
        // material-count change (e.g. "+ Add Material"), destroying and
        // reallocating the WHOLE array from scratch, so this bug hit even
        // pre-existing, already-painted layers, not just the new one.
        // Previously this relied entirely on a later UploadArray() call to
        // overwrite the garbage before anything samples it. Explicitly
        // zero-clearing right here removes that ordering assumption: the
        // array is never in a state where a new material slot's weight
        // channel could read as garbage (observed as "new material painted
        // at ~100% weight everywhere" — see docs/TECH_DEBT.md "Terrain
        // multi-material authoring (Phase 1)").
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, W, H, (GLsizei)layers.size(),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        TerrainCheckGL_("Splatmap::RebuildArrayStorage after glTexImage3D");
        const uint8_t zero[4] = {0, 0, 0, 0};
        glClearTexImage(arrayTex, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        TerrainCheckGL_("Splatmap::RebuildArrayStorage after glClearTexImage");
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        TerrainCheckGL_("Splatmap::RebuildArrayStorage after glTexParameteri x4");
    }

    void UploadArray() {
        if (!arrayTex) { RebuildArrayStorage(); if (!arrayTex) return; }
        glBindTexture(GL_TEXTURE_2D_ARRAY, arrayTex);
        TerrainCheckGL_("Splatmap::UploadArray after glBindTexture");
        for (int i = 0; i < (int)layers.size(); ++i) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, W, H, 1,
                             GL_RGBA, GL_UNSIGNED_BYTE, layers[i].data.data());
            TerrainCheckGL_("Splatmap::UploadArray after glTexSubImage3D (per-layer)");
        }
    }
};

// Paint one material layer onto the splatmap stack at world-space (wx, wz).
// matIdx is unbounded (Phase 1: N materials, not just 4) — redistributes
// lost weight proportionally across every OTHER active material slot in the
// whole stack, not just the 4 sharing matIdx's RGBA layer.
inline void PaintSplatmap(Splatmap& smap, float wx, float wz,
                           float radius, float strength, float dt,
                           int matIdx, float terrainW, float terrainH,
                           BrushFalloff falloff = BrushFalloff::Smooth)
{
    const int numSlots = smap.NumMaterialSlots();
    if (matIdx < 0 || matIdx >= numSlots) return;

    float scaleX = smap.W / terrainW;
    float scaleZ = smap.H / terrainH;
    int   cx     = (int)(wx * scaleX);
    int   cz     = (int)(wz * scaleZ);
    float pixR   = radius * scaleX;
    int   r      = (int)std::ceil(pixR) + 1;

    int x0 = std::max(0, cx - r), x1 = std::min(smap.W - 1, cx + r);
    int z0 = std::max(0, cz - r), z1 = std::min(smap.H - 1, cz + r);

    std::vector<float> ch(numSlots);

    for (int z = z0; z <= z1; z++) {
        for (int x = x0; x <= x1; x++) {
            float dx   = (float)(x - cx), dz = (float)(z - cz);
            float dist = std::sqrt(dx*dx + dz*dz);
            if (dist > pixR) continue;

            float w = CalcFalloff(dist, pixR, falloff) * strength * dt * 2.5f;
            w = std::min(w, 1.f);

            // Read every active slot as float first to avoid per-channel
            // uint8 rounding during the redistribute step.
            for (int i = 0; i < numSlots; i++) ch[i] = smap.GetWeight(x, z, i);

            float prev = ch[matIdx];
            float newV = std::min(prev + w * (1.f - prev), 1.f);
            ch[matIdx] = newV;

            // Redistribute lost weight proportionally across every other
            // material slot in the whole stack (not just its own RGBA group).
            float gain   = newV - prev;
            float others = 0.f;
            for (int i = 0; i < numSlots; i++) if (i != matIdx) others += ch[i];
            if (others > 1e-6f) {
                float ratio = gain / others;
                for (int i = 0; i < numSlots; i++)
                    if (i != matIdx) ch[i] = std::max(ch[i] - ch[i] * ratio, 0.f);
            }

            for (int i = 0; i < numSlots; i++) smap.SetWeight(x, z, i, ch[i]);
        }
    }
    smap.dirty = true;
}
