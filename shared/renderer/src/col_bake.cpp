#include "rco/renderer/col_bake.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <unordered_set>

namespace rco::renderer {

namespace {

struct QuantVtx {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

inline bool operator==(const QuantVtx& a, const QuantVtx& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

struct QuantVtxLess {
    bool operator()(const QuantVtx& a, const QuantVtx& b) const {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    }
};

struct QuantVtxHash {
    size_t operator()(const QuantVtx& v) const {
        size_t h = std::hash<int32_t>{}(v.x);
        h ^= std::hash<int32_t>{}(v.y) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(v.z) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct QuantTriKey {
    QuantVtx a;
    QuantVtx b;
    QuantVtx c;
};

inline bool operator==(const QuantTriKey& lhs, const QuantTriKey& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}

struct QuantTriKeyHash {
    size_t operator()(const QuantTriKey& t) const {
        QuantVtxHash hv;
        size_t h = hv(t.a);
        h ^= hv(t.b) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= hv(t.c) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

inline QuantVtx QuantizePos(const glm::vec3& p) {
    constexpr float kQuant = 100000.0f;
    return {
        (int32_t)std::lround(p.x * kQuant),
        (int32_t)std::lround(p.y * kQuant),
        (int32_t)std::lround(p.z * kQuant),
    };
}

} // namespace

bool ExtractMeshTriangles(const std::string& path,
                          std::vector<std::array<glm::vec3, 3>>& out_tris)
{
    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_PreTransformVertices);
    if (!scene || !scene->mRootNode || scene->mNumMeshes == 0) return false;

    std::unordered_set<QuantTriKey, QuantTriKeyHash> unique;
    unique.reserve((size_t)scene->mNumMeshes * 1024);
    QuantVtxLess lessVtx;

    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh->HasPositions() || !mesh->HasFaces()) continue;
        for (unsigned fi = 0; fi < mesh->mNumFaces; ++fi) {
            const aiFace& face = mesh->mFaces[fi];
            if (face.mNumIndices != 3) continue;
            std::array<glm::vec3, 3> tri;
            std::array<QuantVtx, 3> qtri;
            for (int v = 0; v < 3; ++v) {
                const aiVector3D& p = mesh->mVertices[face.mIndices[v]];
                tri[v] = { p.x, p.y, p.z };
                qtri[v] = QuantizePos(tri[v]);
            }

            if (qtri[0] == qtri[1] || qtri[1] == qtri[2] || qtri[2] == qtri[0])
                continue;

            std::sort(qtri.begin(), qtri.end(), lessVtx);
            QuantTriKey key{qtri[0], qtri[1], qtri[2]};
            if (!unique.insert(key).second)
                continue;

            out_tris.push_back(tri);
        }
    }
    return !out_tris.empty();
}

} // namespace rco::renderer
