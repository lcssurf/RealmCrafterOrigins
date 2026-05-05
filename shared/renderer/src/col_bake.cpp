#include "rco/renderer/col_bake.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace rco::renderer {

bool ExtractMeshTriangles(const std::string& path,
                          std::vector<std::array<glm::vec3, 3>>& out_tris)
{
    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_PreTransformVertices);
    if (!scene || !scene->mRootNode || scene->mNumMeshes == 0) return false;

    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh->HasPositions() || !mesh->HasFaces()) continue;
        for (unsigned fi = 0; fi < mesh->mNumFaces; ++fi) {
            const aiFace& face = mesh->mFaces[fi];
            if (face.mNumIndices != 3) continue;
            std::array<glm::vec3, 3> tri;
            for (int v = 0; v < 3; ++v) {
                const aiVector3D& p = mesh->mVertices[face.mIndices[v]];
                tri[v] = { p.x, p.y, p.z };
            }
            out_tris.push_back(tri);
        }
    }
    return !out_tris.empty();
}

} // namespace rco::renderer
