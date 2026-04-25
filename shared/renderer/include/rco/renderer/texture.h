#pragma once
#include <cstdint>
#include <string>
#include <glm/glm.hpp>
#include <glad/glad.h>

namespace rco::renderer {

struct TextureCreateInfo {
    std::string path;
    bool sRGB         = false;
    bool generateMips = true;
    bool HDR          = false;
    int  minFilter    = GL_LINEAR_MIPMAP_LINEAR;
    int  magFilter    = GL_LINEAR;
};

class Texture2D {
public:
    explicit Texture2D(const TextureCreateInfo& info);
    Texture2D(const Texture2D&)            = delete;
    Texture2D& operator=(Texture2D&&) noexcept;
    Texture2D(Texture2D&&) noexcept;
    ~Texture2D();

    void       Bind(unsigned slot = 0) const;
    unsigned   GetID()             const { return id_; }
    uint64_t   GetBindlessHandle() const { return bindlessHandle_; }
    bool       Valid()             const { return id_ != 0; }
    glm::ivec2 GetSize()           const { return dim_; }

private:
    unsigned   id_             {};
    uint64_t   bindlessHandle_ {};
    glm::ivec2 dim_            {};
};

} // namespace rco::renderer
