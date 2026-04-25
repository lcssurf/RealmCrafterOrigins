#pragma once
#include <glad/glad.h>
#include <string>

namespace rco::renderer {

class Mesh;

void GLAPIENTRY GLerrorCB(GLenum source, GLenum type, GLuint id,
                          GLenum severity, GLsizei length,
                          const GLchar* message, const void* userParam);

void drawFSTexture(GLuint texID);

void blurTextureRGBA32f(GLuint inOutTex, GLuint intermediateTex,
                        GLint width, GLint height, GLint passes, GLint strength);
void blurTextureRGBA16f(GLuint inOutTex, GLuint intermediateTex,
                        GLint width, GLint height, GLint passes, GLint strength);
void blurTextureRG32f  (GLuint inOutTex, GLuint intermediateTex,
                        GLint width, GLint height, GLint passes, GLint strength);
void blurTextureR32f   (GLuint inOutTex, GLuint intermediateTex,
                        GLint width, GLint height, GLint passes, GLint strength);

void convolve_image(GLuint inTex, GLuint outTex, GLint outWidth, GLint outHeight);

void CompileAllShaders();

// Lazy-loaded unit sphere used as light volume mesh in the local lights pass.
Mesh& GetUnitLightSphere();

} // namespace rco::renderer
