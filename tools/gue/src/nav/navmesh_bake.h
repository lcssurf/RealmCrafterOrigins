#pragma once
#include "geo_extract.h"
#include "navmesh_io.h"
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace nav {

// Minimum number of connected polys to keep an island. Smaller islands are removed.
static constexpr int kMinIslandSize = 8;

// Vertex deduplication tolerance (world units).
static constexpr float kVertexEps = 0.01f;

namespace detail {

struct EdgeKey {
    uint32_t a, b; // always a < b
    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};

struct EdgeKeyHash {
    size_t operator()(const EdgeKey& e) const {
        return std::hash<uint64_t>{}((uint64_t)e.a << 32 | e.b);
    }
};

// Returns index of a vertex equal to v within eps, or adds it and returns new index.
inline uint32_t DedupeVertex(std::vector<glm::vec3>& verts,
                              std::unordered_map<uint64_t, uint32_t>& grid,
                              const glm::vec3& v)
{
    // Snap to grid to form a stable key.
    int64_t gx = static_cast<int64_t>(std::round(v.x / kVertexEps));
    int64_t gy = static_cast<int64_t>(std::round(v.y / kVertexEps));
    int64_t gz = static_cast<int64_t>(std::round(v.z / kVertexEps));
    uint64_t key = static_cast<uint64_t>(gx & 0xFFFFF)
                 | (static_cast<uint64_t>(gy & 0xFFFFF) << 20)
                 | (static_cast<uint64_t>(gz & 0xFFFFF) << 40);
    auto it = grid.find(key);
    if (it != grid.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(verts.size());
    verts.push_back(v);
    grid[key] = idx;
    return idx;
}

} // namespace detail

// Bake a NavMesh from a list of candidates.
// Only walkable candidates are included. Adjacency is computed by shared edges.
// Small disconnected islands are removed.
inline NavMesh Bake(const std::vector<NavTriCandidate>& candidates) {
    using namespace detail;

    // --- Deduplicate vertices and build poly list ---
    std::vector<glm::vec3> verts;
    std::vector<NavPoly>   polys;
    std::unordered_map<uint64_t, uint32_t> vertGrid;

    for (const auto& c : candidates) {
        if (!c.walkable) continue;
        NavPoly p;
        for (int i = 0; i < 3; ++i)
            p.v[i] = DedupeVertex(verts, vertGrid, c.v[i]);
        // Skip degenerate triangles (two or more verts collapsed to same index).
        if (p.v[0] == p.v[1] || p.v[1] == p.v[2] || p.v[0] == p.v[2]) continue;
        p.neighbor[0] = p.neighbor[1] = p.neighbor[2] = -1;
        polys.push_back(p);
    }

    // --- Build adjacency via edge → poly map ---
    // Edge key: (min_vert, max_vert). Each edge is shared by at most two polys.
    std::unordered_map<EdgeKey, int32_t, EdgeKeyHash> edgeMap;
    edgeMap.reserve(polys.size() * 3);

    for (int32_t pi = 0; pi < static_cast<int32_t>(polys.size()); ++pi) {
        auto& poly = polys[pi];
        for (int e = 0; e < 3; ++e) {
            uint32_t va = poly.v[e], vb = poly.v[(e + 1) % 3];
            EdgeKey key{ std::min(va, vb), std::max(va, vb) };
            auto it = edgeMap.find(key);
            if (it == edgeMap.end()) {
                edgeMap[key] = pi; // first poly using this edge
            } else {
                int32_t other = it->second;
                // Link both polys.
                poly.neighbor[e] = other;
                // Find the matching edge on the other poly and link back.
                auto& op = polys[other];
                for (int oe = 0; oe < 3; ++oe) {
                    uint32_t ova = op.v[oe], ovb = op.v[(oe + 1) % 3];
                    if (std::min(ova, ovb) == key.a && std::max(ova, ovb) == key.b) {
                        op.neighbor[oe] = pi;
                        break;
                    }
                }
                edgeMap.erase(it); // edge fully resolved
            }
        }
    }

    // --- Flood-fill to remove small islands ---
    int n = static_cast<int>(polys.size());
    std::vector<int> component(n, -1);
    std::vector<int> islandSize;
    std::vector<int> stack;

    for (int start = 0; start < n; ++start) {
        if (component[start] >= 0) continue;
        int comp = static_cast<int>(islandSize.size());
        islandSize.push_back(0);
        stack.push_back(start);
        while (!stack.empty()) {
            int cur = stack.back(); stack.pop_back();
            if (component[cur] >= 0) continue;
            component[cur] = comp;
            ++islandSize[comp];
            for (int e = 0; e < 3; ++e) {
                int nb = polys[cur].neighbor[e];
                if (nb >= 0 && component[nb] < 0)
                    stack.push_back(nb);
            }
        }
    }

    // Keep only polys whose island meets the minimum size.
    NavMesh out;
    out.verts = std::move(verts);
    for (int pi = 0; pi < n; ++pi) {
        int comp = component[pi];
        if (comp >= 0 && islandSize[comp] >= kMinIslandSize)
            out.polys.push_back(polys[pi]);
    }

    return out;
}

} // namespace nav
