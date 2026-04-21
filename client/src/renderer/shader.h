#pragma once
#include <string>
#include <glad/glad.h>
#include <glm/glm.hpp>

namespace rco::renderer {

class Shader {
public:
    Shader() = default;
    ~Shader();

    bool Load(const char* vert_path, const char* frag_path);
    void Use() const;

    void SetInt  (const char* name, int v)              const;
    void SetBool (const char* name, bool v)             const;
    void SetFloat(const char* name, float v)            const;
    void SetVec3 (const char* name, const glm::vec3& v) const;
    void SetVec4 (const char* name, const glm::vec4& v) const;
    void SetMat4 (const char* name, const glm::mat4& m) const;

    GLuint id() const { return id_; }

private:
    GLuint id_ = 0;
    static GLuint CompileStage(GLenum type, const char* src);
};

} // namespace rco::renderer
