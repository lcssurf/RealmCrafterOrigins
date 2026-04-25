#include "rco/renderer/shader.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>

namespace rco::renderer {

std::string Shader::loadFile(const std::string& path) {
    std::string full = shader_dir_ + path;
    std::ifstream ifs(full);
    if (!ifs) {
        std::cout << "Failed to open shader file: " << full << '\n';
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
}

std::string Shader::resolveIncludes(const std::string& src) {
    std::string out;
    std::istringstream ss(src);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find("#include");
        if (pos != std::string::npos) {
            auto q1 = line.find('"', pos);
            auto q2 = line.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) {
                out += line + "\n";
                continue;
            }
            std::string inc     = line.substr(q1 + 1, q2 - q1 - 1);
            std::string content = loadFile(inc);
            out += resolveIncludes(content);
        } else {
            out += line + "\n";
        }
    }
    return out;
}

GLuint Shader::compileShader(GLenum type, const std::string& src,
                             const std::string& preamble,
                             const std::string& path) {
    GLuint shader = glCreateShader(type);

    std::string versionLine;
    std::string rest = src;
    if (auto vp = src.find("#version"); vp != std::string::npos) {
        auto nl     = src.find('\n', vp);
        versionLine = src.substr(0, nl + 1);
        rest        = src.substr(nl + 1);
    }

    std::string finalSrc = versionLine + preamble + rest;
    const GLchar* str = finalSrc.c_str();
    glShaderSource(shader, 1, &str, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[2048];
        glGetShaderInfoLog(shader, 2048, nullptr, infoLog);
        std::cout << "File: " << path << '\n';
        std::cout << "Error compiling shader type " << type << '\n'
                  << infoLog << '\n';
    }
    return shader;
}

Shader::Shader(std::vector<ShaderInfo> shaderInfos) {
    std::vector<GLuint> shaderIDs;
    for (auto& [path, type, preamble] : shaderInfos) {
        std::string raw     = loadFile(path);
        std::string inlined = resolveIncludes(raw);
        GLuint id = compileShader(type, inlined, preamble, path);
        shaderIDs.push_back(id);
    }

    programID = glCreateProgram();
    for (auto id : shaderIDs) glAttachShader(programID, id);
    glLinkProgram(programID);

    std::vector<std::string_view> strs;
    for (const auto& si : shaderInfos) strs.push_back(si.path);
    checkLinkStatus(strs);
    initUniforms();

    for (auto id : shaderIDs) {
        glDetachShader(programID, id);
        glDeleteShader(id);
    }
}

Shader::Shader(Shader&& other) noexcept
    : programID(std::exchange(other.programID, 0)),
      Uniforms(std::move(other.Uniforms)) {}

Shader::~Shader() {
    if (programID != 0) glDeleteProgram(programID);
}

void Shader::initUniforms() {
    GLint count = 0;
    glGetProgramiv(programID, GL_ACTIVE_UNIFORMS, &count);
    if (count == 0) return;

    GLint maxLen = 0;
    glGetProgramiv(programID, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxLen);
    auto name = std::make_unique<char[]>(maxLen);

    for (GLint i = 0; i < count; ++i) {
        GLsizei length = 0, cnt = 0;
        GLenum  type   = GL_NONE;
        glGetActiveUniform(programID, i, maxLen, &length, &cnt, &type, name.get());
        GLint loc = glGetUniformLocation(programID, name.get());
        Uniforms.emplace(std::string(name.get(), length), loc);
    }
}

void Shader::checkLinkStatus(std::vector<std::string_view> files) {
    GLint success;
    glGetProgramiv(programID, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[2048];
        glGetProgramInfoLog(programID, 2048, nullptr, infoLog);
        std::cout << "File(s): ";
        for (auto f : files) std::cout << f << ' ';
        std::cout << "\nFailed to link:\n" << infoLog << '\n';
    }
}

// Helper: look up uniform location; asserts on debug, returns -1 if not found (silently no-ops in release)
static inline GLint loc(const std::unordered_map<std::string, GLint>& u, const std::string& name) {
    auto it = u.find(name);
    assert(it != u.end());
    if (it == u.end()) return -1;
    return it->second;
}

void Shader::SetBool(std::string u, bool v)        const { glProgramUniform1i(programID, loc(Uniforms, u), (int)v); }
void Shader::SetInt(std::string u, int v)          const { glProgramUniform1i(programID, loc(Uniforms, u), v); }
void Shader::SetUInt(std::string u, unsigned v)    const { glProgramUniform1ui(programID, loc(Uniforms, u), v); }
void Shader::SetFloat(std::string u, float v)      const { glProgramUniform1f(programID, loc(Uniforms, u), v); }
void Shader::Set1FloatArray(std::string u, std::span<const float> v)    const { glProgramUniform1fv(programID, loc(Uniforms, u), (GLsizei)v.size(), v.data()); }
void Shader::Set1FloatArray(std::string u, const float* v, GLsizei c)   const { glProgramUniform1fv(programID, loc(Uniforms, u), c, v); }
void Shader::Set2FloatArray(std::string u, std::span<const glm::vec2> v) const { glProgramUniform2fv(programID, loc(Uniforms, u), (GLsizei)v.size(), glm::value_ptr(v.front())); }
void Shader::Set3FloatArray(std::string u, std::span<const glm::vec3> v) const { glProgramUniform3fv(programID, loc(Uniforms, u), (GLsizei)v.size(), glm::value_ptr(v.front())); }
void Shader::Set4FloatArray(std::string u, std::span<const glm::vec4> v) const { glProgramUniform4fv(programID, loc(Uniforms, u), (GLsizei)v.size(), glm::value_ptr(v.front())); }
void Shader::SetIntArray(std::string u, std::span<const int> v)          const { glProgramUniform1iv(programID, loc(Uniforms, u), (GLsizei)v.size(), v.data()); }
void Shader::SetVec2(std::string u, const glm::vec2& v)      const { glProgramUniform2fv(programID, loc(Uniforms, u), 1, glm::value_ptr(v)); }
void Shader::SetVec2(std::string u, float x, float y)        const { glProgramUniform2f(programID, loc(Uniforms, u), x, y); }
void Shader::SetIVec2(std::string u, const glm::ivec2& v)    const { glProgramUniform2iv(programID, loc(Uniforms, u), 1, glm::value_ptr(v)); }
void Shader::SetIVec2(std::string u, int x, int y)           const { glProgramUniform2i(programID, loc(Uniforms, u), x, y); }
void Shader::SetVec3(std::string u, const glm::vec3& v)      const { glProgramUniform3fv(programID, loc(Uniforms, u), 1, glm::value_ptr(v)); }
void Shader::SetVec3(std::string u, float x, float y, float z) const { glProgramUniform3f(programID, loc(Uniforms, u), x, y, z); }
void Shader::SetVec4(std::string u, const glm::vec4& v)      const { glProgramUniform4fv(programID, loc(Uniforms, u), 1, glm::value_ptr(v)); }
void Shader::SetVec4(std::string u, float x, float y, float z, float w) const { glProgramUniform4f(programID, loc(Uniforms, u), x, y, z, w); }
void Shader::SetMat3(std::string u, const glm::mat3& m)      const { glProgramUniformMatrix3fv(programID, loc(Uniforms, u), 1, GL_FALSE, glm::value_ptr(m)); }
void Shader::SetMat4(std::string u, const glm::mat4& m)      const { glProgramUniformMatrix4fv(programID, loc(Uniforms, u), 1, GL_FALSE, glm::value_ptr(m)); }
void Shader::SetHandle(std::string u, uint64_t h)            const { glProgramUniformHandleui64ARB(programID, loc(Uniforms, u), h); }
void Shader::SetHandleArray(std::string u, std::span<const uint64_t> h) const { glProgramUniformHandleui64vARB(programID, loc(Uniforms, u), (GLsizei)h.size(), h.data()); }

} // namespace rco::renderer
