#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cstdint>
#include <fstream>

namespace nav {

static constexpr uint32_t kNavMeshMagic   = 0x52'4E'41'56u; // "RNAV"
static constexpr uint32_t kNavMeshVersion = 1;

struct NavPoly {
    uint32_t v[3];           // vertex indices
    int32_t  neighbor[3];    // adjacent poly per edge, -1 = boundary
};

struct NavMesh {
    std::vector<glm::vec3> verts;
    std::vector<NavPoly>   polys;

    bool Save(const std::string& path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        auto wr = [&](const void* d, size_t n){ f.write(static_cast<const char*>(d), n); };
        auto wru32 = [&](uint32_t v){ wr(&v, 4); };
        auto wri32 = [&](int32_t  v){ wr(&v, 4); };
        auto wrf32 = [&](float    v){ wr(&v, 4); };

        wru32(kNavMeshMagic);
        wru32(kNavMeshVersion);
        wru32(static_cast<uint32_t>(verts.size()));
        for (auto& v : verts) { wrf32(v.x); wrf32(v.y); wrf32(v.z); }
        wru32(static_cast<uint32_t>(polys.size()));
        for (auto& p : polys) {
            wru32(p.v[0]); wru32(p.v[1]); wru32(p.v[2]);
            wri32(p.neighbor[0]); wri32(p.neighbor[1]); wri32(p.neighbor[2]);
        }
        return !!f;
    }

    bool Load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        auto rd = [&](void* d, size_t n){ f.read(static_cast<char*>(d), n); };
        auto rdu32 = [&](uint32_t& v){ rd(&v, 4); };
        auto rdi32 = [&](int32_t&  v){ rd(&v, 4); };
        auto rdf32 = [&](float&    v){ rd(&v, 4); };

        uint32_t magic, version;
        rdu32(magic);   if (magic   != kNavMeshMagic)   return false;
        rdu32(version); if (version != kNavMeshVersion) return false;

        uint32_t nv; rdu32(nv);
        verts.resize(nv);
        for (auto& v : verts) { rdf32(v.x); rdf32(v.y); rdf32(v.z); }

        uint32_t np; rdu32(np);
        polys.resize(np);
        for (auto& p : polys) {
            rdu32(p.v[0]); rdu32(p.v[1]); rdu32(p.v[2]);
            rdi32(p.neighbor[0]); rdi32(p.neighbor[1]); rdi32(p.neighbor[2]);
        }
        return !!f;
    }
};

} // namespace nav
