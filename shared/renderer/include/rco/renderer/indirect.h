#pragma once
#include <glad/glad.h>

namespace rco::renderer {

struct DrawElementsIndirectCommand {
    GLuint count         = 0;
    GLuint instanceCount = 0;
    GLuint firstIndex    = 0;
    GLuint baseVertex    = 0;
    GLuint baseInstance  = 0;
};

struct DrawArraysIndirectCommand {
    GLuint count         = 0;
    GLuint instanceCount = 0;
    GLuint first         = 0;
    GLuint baseInstance  = 0;
};

} // namespace rco::renderer
