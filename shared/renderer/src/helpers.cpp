#include "rco/renderer/helpers.h"
#include "rco/renderer/mesh.h"
#include "rco/renderer/shader.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <iostream>
#include <span>
#include <vector>

namespace rco::renderer {

void GLAPIENTRY GLerrorCB(GLenum source, GLenum type, GLuint id,
                          GLenum severity, GLsizei /*length*/,
                          const GLchar* message, const void* /*userParam*/) {
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204) return;

    std::cout << "---------------\n";
    std::cout << "Debug message (" << id << "): " << message << '\n';

    switch (source) {
    case GL_DEBUG_SOURCE_API:             std::cout << "Source: API"; break;
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   std::cout << "Source: Window Manager"; break;
    case GL_DEBUG_SOURCE_SHADER_COMPILER: std::cout << "Source: Shader Compiler"; break;
    case GL_DEBUG_SOURCE_THIRD_PARTY:     std::cout << "Source: Third Party"; break;
    case GL_DEBUG_SOURCE_APPLICATION:     std::cout << "Source: Application"; break;
    case GL_DEBUG_SOURCE_OTHER:           std::cout << "Source: Other"; break;
    } std::cout << '\n';

    switch (type) {
    case GL_DEBUG_TYPE_ERROR:               std::cout << "Type: Error"; break;
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: std::cout << "Type: Deprecated Behaviour"; break;
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  std::cout << "Type: Undefined Behaviour"; break;
    case GL_DEBUG_TYPE_PORTABILITY:         std::cout << "Type: Portability"; break;
    case GL_DEBUG_TYPE_PERFORMANCE:         std::cout << "Type: Performance"; break;
    case GL_DEBUG_TYPE_MARKER:              std::cout << "Type: Marker"; break;
    case GL_DEBUG_TYPE_PUSH_GROUP:          std::cout << "Type: Push Group"; break;
    case GL_DEBUG_TYPE_POP_GROUP:           std::cout << "Type: Pop Group"; break;
    case GL_DEBUG_TYPE_OTHER:               std::cout << "Type: Other"; break;
    } std::cout << '\n';

    switch (severity) {
    case GL_DEBUG_SEVERITY_HIGH:         std::cout << "Severity: high"; break;
    case GL_DEBUG_SEVERITY_MEDIUM:       std::cout << "Severity: medium"; break;
    case GL_DEBUG_SEVERITY_LOW:          std::cout << "Severity: low"; break;
    case GL_DEBUG_SEVERITY_NOTIFICATION: std::cout << "Severity: notification"; break;
    } std::cout << "\n\n";
}

void drawFSTexture(GLuint texID) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindTextureUnit(0, texID);
    auto& s = Shader::shaders["fstexture"];
    s->Bind();
    s->SetInt("u_texture", 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

namespace {
    void blurTextureBase(GLuint inOutTex, GLuint intermediateTexture,
                         GLint width, GLint height,
                         GLint passes, GLint strength,
                         std::span<std::string> shaderNames, GLenum imageFormat) {
        if (strength > (GLint)shaderNames.size() || strength <= 1) return;

        auto& shader = Shader::shaders[shaderNames[strength - 1]];
        shader->Bind();
        shader->SetIVec2("u_texSize", width, height);
        shader->SetInt("u_inTex", 0);
        shader->SetInt("u_outTex", 0);

        const int xgroups = (width  + 7) / 8;
        const int ygroups = (height + 7) / 8;

        bool horizontal = false;
        for (int i = 0; i < passes * 2; ++i) {
            glBindTextureUnit(0, inOutTex);
            glBindImageTexture(0, intermediateTexture, 0, false, 0, GL_WRITE_ONLY, imageFormat);
            shader->SetBool("u_horizontal", horizontal);
            glDispatchCompute(xgroups, ygroups, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            std::swap(inOutTex, intermediateTexture);
            horizontal = !horizontal;
        }
    }
}

void blurTextureRGBA32f(GLuint io, GLuint im, GLint w, GLint h, GLint p, GLint s) {
    std::string names[] = {"gaussianRGBA32f_blur1","gaussianRGBA32f_blur2","gaussianRGBA32f_blur3",
                           "gaussianRGBA32f_blur4","gaussianRGBA32f_blur5","gaussianRGBA32f_blur6"};
    blurTextureBase(io, im, w, h, p, s, names, GL_RGBA32F);
}
void blurTextureRGBA16f(GLuint io, GLuint im, GLint w, GLint h, GLint p, GLint s) {
    std::string names[] = {"gaussianRGBA16f_blur1","gaussianRGBA16f_blur2","gaussianRGBA16f_blur3",
                           "gaussianRGBA16f_blur4","gaussianRGBA16f_blur5","gaussianRGBA16f_blur6"};
    blurTextureBase(io, im, w, h, p, s, names, GL_RGBA16F);
}
void blurTextureRG32f(GLuint io, GLuint im, GLint w, GLint h, GLint p, GLint s) {
    std::string names[] = {"gaussian_blur1","gaussian_blur2","gaussian_blur3",
                           "gaussian_blur4","gaussian_blur5","gaussian_blur6"};
    blurTextureBase(io, im, w, h, p, s, names, GL_RG32F);
}
void blurTextureR32f(GLuint io, GLuint im, GLint w, GLint h, GLint p, GLint s) {
    std::string names[] = {"gaussian32f_blur1","gaussian32f_blur2","gaussian32f_blur3",
                           "gaussian32f_blur4","gaussian32f_blur5","gaussian32f_blur6"};
    blurTextureBase(io, im, w, h, p, s, names, GL_R32F);
}

void convolve_image(GLuint inTex, GLuint outTex, GLint outW, GLint outH) {
    auto& s = Shader::shaders["convolve_image"];
    s->Bind();
    s->SetInt("u_environment", 0);
    s->SetInt("u_outTex", 0);
    const int xgroups = (outW + 7) / 8;
    const int ygroups = (outH + 7) / 8;
    glBindTextureUnit(0, inTex);
    glBindImageTexture(0, outTex, 0, false, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(xgroups, ygroups, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

Mesh& GetUnitLightSphere() {
    static Mesh sphere = []() {
        const int rings   = 10;
        const int sectors = 18;
        const float R = 1.0f / (float)(rings   - 1);
        const float S = 1.0f / (float)(sectors - 1);
        const float PI     = glm::pi<float>();
        const float TWO_PI = 2.0f * PI;

        std::vector<Vertex> verts;
        verts.reserve((size_t)rings * sectors);
        for (int r = 0; r < rings; ++r) {
            for (int s = 0; s < sectors; ++s) {
                float y = glm::sin(-PI / 2.0f + PI * r * R);
                float x = glm::cos(TWO_PI * s * S) * glm::sin(PI * r * R);
                float z = glm::sin(TWO_PI * s * S) * glm::sin(PI * r * R);
                Vertex v;
                v.position = { x, y, z };
                v.normal   = { x, y, z };
                v.uv       = { s * S, r * R };
                verts.push_back(v);
            }
        }

        std::vector<uint32_t> idx;
        idx.reserve((size_t)(rings - 1) * (sectors - 1) * 6);
        for (int r = 0; r < rings - 1; ++r) {
            for (int s = 0; s < sectors - 1; ++s) {
                idx.push_back((uint32_t)(r * sectors + s));
                idx.push_back((uint32_t)(r * sectors + (s + 1)));
                idx.push_back((uint32_t)((r + 1) * sectors + (s + 1)));

                idx.push_back((uint32_t)(r * sectors + s));
                idx.push_back((uint32_t)((r + 1) * sectors + (s + 1)));
                idx.push_back((uint32_t)((r + 1) * sectors + s));
            }
        }

        Material mat{};
        return Mesh(verts, idx, mat);
    }();
    return sphere;
}

} // namespace rco::renderer
