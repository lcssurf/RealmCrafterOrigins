#include "renderer/model.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <glm/glm.hpp>
#include <cstdio>
#include <cstring>

namespace rco::renderer {

// ---------------------------------------------------------------------------
// Placeholder box: 0.5 × 1.8 × 0.3, feet at y=0
// ---------------------------------------------------------------------------
static const float kBoxVerts[] = {
    // pos(3)  normal(3)  uv(2)   — 24 vertices (4 per face × 6 faces)
    // Front (+Z)
    -0.25f, 0.00f, 0.15f,  0, 0, 1,  0,0,
     0.25f, 0.00f, 0.15f,  0, 0, 1,  1,0,
     0.25f, 1.80f, 0.15f,  0, 0, 1,  1,1,
    -0.25f, 1.80f, 0.15f,  0, 0, 1,  0,1,
    // Back (-Z)
     0.25f, 0.00f,-0.15f,  0, 0,-1,  0,0,
    -0.25f, 0.00f,-0.15f,  0, 0,-1,  1,0,
    -0.25f, 1.80f,-0.15f,  0, 0,-1,  1,1,
     0.25f, 1.80f,-0.15f,  0, 0,-1,  0,1,
    // Left (-X)
    -0.25f, 0.00f,-0.15f, -1, 0, 0,  0,0,
    -0.25f, 0.00f, 0.15f, -1, 0, 0,  1,0,
    -0.25f, 1.80f, 0.15f, -1, 0, 0,  1,1,
    -0.25f, 1.80f,-0.15f, -1, 0, 0,  0,1,
    // Right (+X)
     0.25f, 0.00f, 0.15f,  1, 0, 0,  0,0,
     0.25f, 0.00f,-0.15f,  1, 0, 0,  1,0,
     0.25f, 1.80f,-0.15f,  1, 0, 0,  1,1,
     0.25f, 1.80f, 0.15f,  1, 0, 0,  0,1,
    // Top (+Y)
    -0.25f, 1.80f, 0.15f,  0, 1, 0,  0,0,
     0.25f, 1.80f, 0.15f,  0, 1, 0,  1,0,
     0.25f, 1.80f,-0.15f,  0, 1, 0,  1,1,
    -0.25f, 1.80f,-0.15f,  0, 1, 0,  0,1,
    // Bottom (-Y)
    -0.25f, 0.00f,-0.15f,  0,-1, 0,  0,0,
     0.25f, 0.00f,-0.15f,  0,-1, 0,  1,0,
     0.25f, 0.00f, 0.15f,  0,-1, 0,  1,1,
    -0.25f, 0.00f, 0.15f,  0,-1, 0,  0,1,
};
static const unsigned kBoxIdx[] = {
     0, 1, 2,  2, 3, 0,
     4, 5, 6,  6, 7, 4,
     8, 9,10, 10,11, 8,
    12,13,14, 14,15,12,
    16,17,18, 18,19,16,
    20,21,22, 22,23,20,
};

// ---------------------------------------------------------------------------

Model::~Model() { Destroy(); }

void Model::FreeMesh(SubMesh& m) {
    if (m.vao) glDeleteVertexArrays(1, &m.vao);
    if (m.vbo) glDeleteBuffers(1, &m.vbo);
    if (m.ebo) glDeleteBuffers(1, &m.ebo);
    m.vao = m.vbo = m.ebo = 0;
}

void Model::Destroy() {
    for (auto& m : meshes_) FreeMesh(m);
    meshes_.clear();
}

void Model::GeneratePlaceholder() {
    SubMesh m;
    m.base_color = {0.45f, 0.60f, 0.80f};
    m.idx_count  = static_cast<int>(sizeof(kBoxIdx) / sizeof(kBoxIdx[0]));

    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ebo);

    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kBoxVerts), kBoxVerts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kBoxIdx), kBoxIdx, GL_STATIC_DRAW);

    constexpr int stride = 8 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(24));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    meshes_.push_back(m);
}

bool Model::Load(const char* path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate   |
        aiProcess_FlipUVs       |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace);

    if (!scene || !scene->mRootNode ||
        (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::fprintf(stderr, "[model] Cannot load '%s': %s — using placeholder\n",
            path, importer.GetErrorString());
        GeneratePlaceholder();
        return false;
    }

    std::string p(path);
    size_t slash = p.find_last_of("/\\");
    directory_ = (slash != std::string::npos) ? p.substr(0, slash) : ".";

    ProcessNode(scene->mRootNode, scene);
    return true;
}

void Model::ProcessNode(aiNode* node, const aiScene* scene) {
    for (unsigned i = 0; i < node->mNumMeshes; ++i)
        meshes_.push_back(ProcessMesh(scene->mMeshes[node->mMeshes[i]], scene));
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        ProcessNode(node->mChildren[i], scene);
}

SubMesh Model::ProcessMesh(aiMesh* mesh, const aiScene* scene) {
    // Interleaved: pos(3) normal(3) uv(2)
    std::vector<float>    verts;
    std::vector<unsigned> indices;
    verts.reserve(mesh->mNumVertices * 8);

    for (unsigned i = 0; i < mesh->mNumVertices; ++i) {
        verts.push_back(mesh->mVertices[i].x);
        verts.push_back(mesh->mVertices[i].y);
        verts.push_back(mesh->mVertices[i].z);

        if (mesh->HasNormals()) {
            verts.push_back(mesh->mNormals[i].x);
            verts.push_back(mesh->mNormals[i].y);
            verts.push_back(mesh->mNormals[i].z);
        } else {
            verts.push_back(0.f); verts.push_back(1.f); verts.push_back(0.f);
        }

        if (mesh->mTextureCoords[0]) {
            verts.push_back(mesh->mTextureCoords[0][i].x);
            verts.push_back(mesh->mTextureCoords[0][i].y);
        } else {
            verts.push_back(0.f); verts.push_back(0.f);
        }
    }

    for (unsigned i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace& face = mesh->mFaces[i];
        for (unsigned j = 0; j < face.mNumIndices; ++j)
            indices.push_back(face.mIndices[j]);
    }

    // Material base color
    SubMesh sm;
    sm.idx_count  = static_cast<int>(indices.size());
    sm.base_color = {0.72f, 0.68f, 0.60f};

    if (mesh->mMaterialIndex < scene->mNumMaterials) {
        aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
        aiColor4D  col;
        if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, col) == AI_SUCCESS)
            sm.base_color = {col.r, col.g, col.b};
    }

    glGenVertexArrays(1, &sm.vao);
    glGenBuffers(1, &sm.vbo);
    glGenBuffers(1, &sm.ebo);

    glBindVertexArray(sm.vao);
    glBindBuffer(GL_ARRAY_BUFFER, sm.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sm.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned), indices.data(), GL_STATIC_DRAW);

    constexpr int stride = 8 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(24));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    return sm;
}

void Model::Draw(Shader& shader) const {
    for (const auto& m : meshes_) {
        shader.SetVec3("uBaseColor", m.base_color);
        glBindVertexArray(m.vao);
        glDrawElements(GL_TRIANGLES, m.idx_count, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }
}

} // namespace rco::renderer
