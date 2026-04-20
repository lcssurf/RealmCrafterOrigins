#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include "renderer/shader.h"

struct aiNode;
struct aiScene;
struct aiMesh;

namespace rco::renderer {

struct SubMesh {
    GLuint    vao        = 0;
    GLuint    vbo        = 0;
    GLuint    ebo        = 0;
    int       idx_count  = 0;
    glm::vec3 base_color = {0.72f, 0.68f, 0.60f};
};

class Model {
public:
    ~Model();

    // Returns true if file loaded; false uses a placeholder box.
    bool Load(const char* path);
    void Draw(Shader& shader) const;
    void Destroy();
    bool IsLoaded() const { return !meshes_.empty(); }

private:
    std::vector<SubMesh> meshes_;
    std::string          directory_;

    void ProcessNode(aiNode* node, const aiScene* scene);
    SubMesh ProcessMesh(aiMesh* mesh, const aiScene* scene);
    void GeneratePlaceholder();
    static void FreeMesh(SubMesh& m);
};

} // namespace rco::renderer
