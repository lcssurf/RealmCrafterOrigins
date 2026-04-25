#pragma once
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

namespace rco::renderer {

struct ShaderInfo {
    std::string path;      // relative to shader_dir_
    GLenum      type;
    std::string preamble;  // injected before source (e.g. "#define HAS_SKINNING\n")

    ShaderInfo(std::string p, GLenum t, std::string pre = "")
        : path(std::move(p)), type(t), preamble(std::move(pre)) {}
};

class Shader {
public:
    Shader() = default;
    explicit Shader(std::vector<ShaderInfo> shaderInfos);
    Shader(Shader&& other) noexcept;
    ~Shader();

    void Bind() const { glUseProgram(programID); }

    void SetBool(std::string uniform, bool value)                           const;
    void SetInt(std::string uniform, int value)                             const;
    void SetUInt(std::string uniform, unsigned int value)                   const;
    void SetFloat(std::string uniform, float value)                         const;
    void Set1FloatArray(std::string uniform, std::span<const float> value)  const;
    void Set1FloatArray(std::string uniform, const float* value, GLsizei c) const;
    void Set2FloatArray(std::string uniform, std::span<const glm::vec2> v)  const;
    void Set3FloatArray(std::string uniform, std::span<const glm::vec3> v)  const;
    void Set4FloatArray(std::string uniform, std::span<const glm::vec4> v)  const;
    void SetIntArray(std::string uniform, std::span<const int> value)       const;
    void SetVec2(std::string uniform, const glm::vec2& value)               const;
    void SetVec2(std::string uniform, float x, float y)                     const;
    void SetIVec2(std::string uniform, const glm::ivec2& value)             const;
    void SetIVec2(std::string uniform, int x, int y)                        const;
    void SetVec3(std::string uniform, const glm::vec3& value)               const;
    void SetVec3(std::string uniform, float x, float y, float z)            const;
    void SetVec4(std::string uniform, const glm::vec4& value)               const;
    void SetVec4(std::string uniform, float x, float y, float z, float w)   const;
    void SetMat3(std::string uniform, const glm::mat3& mat)                 const;
    void SetMat4(std::string uniform, const glm::mat4& mat)                 const;
    void SetHandle(std::string uniform, uint64_t handle)                    const;
    void SetHandleArray(std::string uniform, std::span<const uint64_t> h)   const;

    static inline std::unordered_map<std::string, std::optional<Shader>> shaders;

    static inline std::string shader_dir_ = "shaders/";
    static void SetShaderDir(const std::string& dir) { shader_dir_ = dir; }

private:
    GLuint programID { 0 };
    std::unordered_map<std::string, GLint> Uniforms;

    static std::string loadFile(const std::string& path);
    static std::string resolveIncludes(const std::string& src);

    GLuint compileShader(GLenum type, const std::string& src,
                         const std::string& preamble, const std::string& path);
    void   initUniforms();
    void   checkLinkStatus(std::vector<std::string_view> files);
};

} // namespace rco::renderer
