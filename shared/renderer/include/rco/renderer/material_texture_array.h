#pragma once
#include <glad/glad.h>
#include <vector>
#include <cstdint>

namespace rco::renderer {

// MaterialTextureArray — GL_TEXTURE_2D_ARRAY holding one texture role
// (albedo, normal, roughness, ao or height) across every configured terrain
// material, so the generalized N-material shader path (terrainGBufferExt.fs)
// samples a single unit regardless of material count N. Shared between the
// GUE editor (tools/gue/src/terrain/editable_terrain.cpp) and the game
// client (client/src/renderer/terrain/terrain.cpp) — both build one set of
// these (albedo/normal/roughness/ao/height) once a terrain has more than 4
// materials. See docs/TECH_DEBT.md "Terrain multi-material authoring
// (Phase 1)".
//
// All layers share one fixed resolution: each material's already-loaded 2D
// GL texture is read back (glGetTexImage) and nearest-neighbour resampled
// to kRes×kRes before upload, so materials authored at different native
// resolutions can still share one array.
struct MaterialTextureArray {
    static constexpr int kRes = 512; // fixed per-layer resolution

    GLuint tex    = 0;
    int    layers = 0;

    void Resize(int numLayers, bool srgb) {
        Destroy();
        if (numLayers <= 0) return;
        layers = numLayers;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        GLenum internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, internal, kRes, kRes, layers, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    // Reads back `srcTex` (a standalone 2D texture already loaded elsewhere)
    // and uploads a resampled copy into `layer`. srcTex == 0 fills the layer
    // with opaque white (matches the meaning of a "0 = use default" GL
    // texture id elsewhere in this codebase).
    void SetLayerFromGLTexture(int layer, GLuint srcTex) {
        if (layer < 0 || layer >= layers || tex == 0) return;
        std::vector<uint8_t> px((size_t)kRes * kRes * 4);

        if (srcTex == 0) {
            std::fill(px.begin(), px.end(), (uint8_t)255);
        } else {
            GLint sw = 0, sh = 0;
            glBindTexture(GL_TEXTURE_2D, srcTex);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &sw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &sh);
            if (sw <= 0 || sh <= 0) {
                std::fill(px.begin(), px.end(), (uint8_t)255);
            } else {
                std::vector<uint8_t> src((size_t)sw * sh * 4);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, src.data());
                for (int y = 0; y < kRes; ++y) {
                    int sy = y * sh / kRes;
                    for (int x = 0; x < kRes; ++x) {
                        int sx = x * sw / kRes;
                        const uint8_t* sp = &src[((size_t)sy * sw + sx) * 4];
                        uint8_t*       dp = &px[((size_t)y * kRes + x) * 4];
                        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
                    }
                }
            }
        }

        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, kRes, kRes, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }

    void GenerateMipmaps() {
        if (!tex) return;
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    }

    void Destroy() {
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
        layers = 0;
    }
};

// SplatWeightArray — GL_TEXTURE_2D_ARRAY holding the splatmap's weight
// layers (each layer's RGBA channels = weights for 4 materials), built from
// already-decoded W*H*4 uint8 layer buffers (e.g. read from the RSPN
// multi-layer splatmap.bin format). Shared between the GUE editor (which
// also keeps a richer, paintable Splatmap — see tools/gue/src/terrain/
// splatmap.h) and the game client (which only needs to display the baked
// result, not edit it).
struct SplatWeightArray {
    GLuint tex = 0;
    int    numGroups = 0;

    // `layerData` must contain exactly numLayers buffers of W*H*4 bytes each
    // (RGBA8, one byte per weight channel).
    void Build(int w, int h, const std::vector<std::vector<uint8_t>>& layerData) {
        Destroy();
        numGroups = (int)layerData.size();
        if (numGroups <= 0 || w <= 0 || h <= 0) { numGroups = 0; return; }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w, h, numGroups,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        const uint8_t zero[4] = {0, 0, 0, 0};
        glClearTexImage(tex, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        for (int i = 0; i < numGroups; ++i)
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, w, h, 1,
                             GL_RGBA, GL_UNSIGNED_BYTE, layerData[i].data());
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    void Destroy() {
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
        numGroups = 0;
    }
};

} // namespace rco::renderer
