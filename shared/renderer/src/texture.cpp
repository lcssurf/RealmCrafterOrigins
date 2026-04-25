#include "rco/renderer/texture.h"
#include <cassert>
#include <filesystem>
#include <stb_image.h>

namespace rco::renderer {

Texture2D::Texture2D(const TextureCreateInfo& createInfo) {
    assert(!(createInfo.sRGB && createInfo.HDR));
    stbi_set_flip_vertically_on_load(true);

    std::string tex = createInfo.path;
    bool hasTex = std::filesystem::exists(tex) &&
                  std::filesystem::is_regular_file(tex);
    if (!hasTex) return;

    int n;
    auto pixels = stbi_loadf(tex.c_str(), &dim_.x, &dim_.y, &n, 4);
    assert(pixels != nullptr);

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
    glTextureSubImage2D(id_, 0, 0, 0, dim_.x, dim_.y, GL_RGBA, GL_FLOAT, pixels);
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
