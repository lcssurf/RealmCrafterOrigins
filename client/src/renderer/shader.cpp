#include "renderer/shader.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <glm/gtc/type_ptr.hpp>

namespace rco::renderer {

static std::string ReadFile(const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[shader] Cannot open: %s\n", path);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Shader::~Shader() {
    if (id_) glDeleteProgram(id_);
}

bool Shader::Load(const char* vert_path, const char* frag_path) {
    std::string vs = ReadFile(vert_path);
    std::string fs = ReadFile(frag_path);
    if (vs.empty() || fs.empty()) return false;

    GLuint v = CompileStage(GL_VERTEX_SHADER,   vs.c_str());
    GLuint f = CompileStage(GL_FRAGMENT_SHADER, fs.c_str());
    if (!v || !f) { glDeleteShader(v); glDeleteShader(f); return false; }

    id_ = glCreateProgram();
    glAttachShader(id_, v);
    glAttachShader(id_, f);
    glLinkProgram(id_);

    GLint ok;
    glGetProgramiv(id_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(id_, 512, nullptr, log);
        std::fprintf(stderr, "[shader] Link error: %s\n", log);
        glDeleteProgram(id_);
        id_ = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return id_ != 0;
}

GLuint Shader::CompileStage(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        std::fprintf(stderr, "[shader] Compile error (%s): %s\n",
            type == GL_VERTEX_SHADER ? "vert" : "frag", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void Shader::Use() const { glUseProgram(id_); }

void Shader::SetInt  (const char* n, int v)              const { glUniform1i (glGetUniformLocation(id_, n), v); }
void Shader::SetBool (const char* n, bool v)             const { glUniform1i (glGetUniformLocation(id_, n), (int)v); }
void Shader::SetFloat(const char* n, float v)            const { glUniform1f (glGetUniformLocation(id_, n), v); }
void Shader::SetVec3 (const char* n, const glm::vec3& v) const { glUniform3fv(glGetUniformLocation(id_, n), 1, glm::value_ptr(v)); }
void Shader::SetVec4 (const char* n, const glm::vec4& v) const { glUniform4fv(glGetUniformLocation(id_, n), 1, glm::value_ptr(v)); }
void Shader::SetMat4 (const char* n, const glm::mat4& m) const { glUniformMatrix4fv(glGetUniformLocation(id_, n), 1, GL_FALSE, glm::value_ptr(m)); }

} // namespace rco::renderer
