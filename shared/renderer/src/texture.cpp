#include "rco/renderer/texture.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <cmath>
#include <stb_image.h>

namespace rco::renderer {

Texture2D::Texture2D(const TextureCreateInfo& createInfo) {
    assert(!(createInfo.sRGB && createInfo.HDR));
    stbi_set_flip_vertically_on_load(true);

    std::string tex = createInfo.path;
    bool hasTex = std::filesystem::exists(tex) &&
                  std::filesystem::is_regular_file(tex);
    if (!hasTex) {
        std::fprintf(stderr, "[tex] file not found: %s\n", tex.c_str());
        return;
    }

    int n = 0;
    void* pixels = nullptr;
    GLenum uploadType = GL_UNSIGNED_BYTE;
    if (createInfo.HDR) {
        pixels = stbi_loadf(tex.c_str(), &dim_.x, &dim_.y, &n, 4);
        uploadType = GL_FLOAT;
    } else {
        pixels = stbi_load(tex.c_str(), &dim_.x, &dim_.y, &n, 4);
        uploadType = GL_UNSIGNED_BYTE;
    }
    if (!pixels || dim_.x <= 0 || dim_.y <= 0) {
        std::fprintf(stderr, "[tex] failed to load: %s (%s)\n",
                     tex.c_str(), stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        if (pixels) stbi_image_free(pixels);
        return;
    }

    if (createInfo.HDR) {
        // Some HDR skies carry extreme solar peaks or invalid values. When
        // uploaded to RGBA16F these can become Inf/NaN and show as black
        // blocks/banding around the sun in cubemap conversions.
        float* fp = static_cast<float*>(pixels);
        const size_t count = static_cast<size_t>(dim_.x) * static_cast<size_t>(dim_.y) * 4u;
        constexpr float kHalfSafeMax = 64000.0f; // below half-float max (65504)
        size_t fixedNonFinite = 0;
        size_t fixedNegative  = 0;
        size_t clampedHigh    = 0;
        for (size_t i = 0; i < count; ++i) {
            float v = fp[i];
            if (!std::isfinite(v)) {
                fp[i] = 0.0f;
                ++fixedNonFinite;
                continue;
            }
            if (v < 0.0f) {
                fp[i] = 0.0f;
                ++fixedNegative;
                continue;
            }
            if (v > kHalfSafeMax) {
                fp[i] = kHalfSafeMax;
                ++clampedHigh;
            }
        }
        if (fixedNonFinite || fixedNegative || clampedHigh) {
            std::fprintf(stderr,
                         "[tex] HDR sanitized '%s': naninf=%zu neg=%zu high=%zu\n",
                         tex.c_str(), fixedNonFinite, fixedNegative, clampedHigh);
        }
    }

    glCreateTextures(GL_TEXTURE_2D, 1, &id_);

    GLfloat a;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &a);
    glTextureParameterf(id_, GL_TEXTURE_MAX_ANISOTROPY, a);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, createInfo.minFilter);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, createInfo.magFilter);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_REPEAT);

    GLuint levels = 1;
    if (createInfo.generateMips) {
        levels = (GLuint)glm::ceil(glm::log2((float)glm::min(dim_.x, dim_.y)));
    }

    const GLenum internalFormat = createInfo.sRGB ? GL_SRGB8_ALPHA8
                                 : createInfo.HDR  ? GL_RGBA16F
                                                    : GL_RGBA8;
    glTextureStorage2D(id_, levels, internalFormat, dim_.x, dim_.y);
    glTextureSubImage2D(id_, 0, 0, 0, dim_.x, dim_.y, GL_RGBA, uploadType, pixels);
    stbi_image_free(pixels);

    if (createInfo.generateMips) glGenerateTextureMipmap(id_);

    bindlessHandle_ = glGetTextureHandleARB(id_);
    glMakeTextureHandleResidentARB(bindlessHandle_);
}

Texture2D& Texture2D::operator=(Texture2D&& rhs) noexcept {
    if (&rhs == this) return *this;
    return *new (this) Texture2D(std::move(rhs));
}

Texture2D::Texture2D(Texture2D&& rhs) noexcept {
    id_             = std::exchange(rhs.id_, 0);
    dim_            = rhs.dim_;
    bindlessHandle_ = std::exchange(rhs.bindlessHandle_, 0);
}

Texture2D::~Texture2D() { glDeleteTextures(1, &id_); }

void Texture2D::Bind(unsigned slot) const { glBindTextureUnit(slot, id_); }

} // namespace rco::renderer
