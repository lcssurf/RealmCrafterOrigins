#include "renderer/skybox.h"
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

namespace rco::renderer {

static const float kVerts[] = {
    -1,-1,-1,  1,-1,-1,  1, 1,-1, -1, 1,-1,
    -1,-1, 1, -1, 1, 1,  1, 1, 1,  1,-1, 1,
    -1, 1,-1,  1, 1,-1,  1, 1, 1, -1, 1, 1,
    -1,-1,-1, -1,-1, 1,  1,-1, 1,  1,-1,-1,
    -1,-1,-1, -1, 1,-1, -1, 1, 1, -1,-1, 1,
     1,-1,-1,  1,-1, 1,  1, 1, 1,  1, 1,-1
};
static const unsigned kIdx[] = {
     0, 1, 2, 2, 3, 0,   4, 5, 6, 6, 7, 4,
     8, 9,10,10,11, 8,  12,13,14,14,15,12,
    16,17,18,18,19,16,  20,21,22,22,23,20
};

bool Skybox::Init(const char* shader_dir) {
    char vert[256], frag[256];
    std::snprintf(vert, sizeof(vert), "%s/skybox.vert", shader_dir);
    std::snprintf(frag, sizeof(frag), "%s/skybox.frag", shader_dir);
    if (!shader_.Load(vert, frag)) return false;

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kIdx), kIdx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    return true;
}

void Skybox::Render(const glm::mat4& view, const glm::mat4& proj) {
    glDepthFunc(GL_LEQUAL);
    shader_.Use();
    glm::mat4 v = glm::mat4(glm::mat3(view));
    shader_.SetMat4("uView",       v);
    shader_.SetMat4("uProj",       proj);
    shader_.SetVec3("uSunDir",     sun_dir);
    shader_.SetVec3("uSkyTop",     sky_top);
    shader_.SetVec3("uSkyHorizon", sky_horizon);
    shader_.SetVec3("uSunColor",   sun_color);
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glDepthFunc(GL_LESS);
}

void Skybox::Destroy() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (ebo_) glDeleteBuffers(1, &ebo_);
    vao_ = vbo_ = ebo_ = 0;
}

} // namespace rco::renderer
