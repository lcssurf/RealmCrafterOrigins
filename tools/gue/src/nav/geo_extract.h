#pragma once
#include "../terrain/heightmap.h"
#include "navmesh_io.h"
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

namespace nav {

struct NavTriCandidate {
    glm::vec3 v[3];
    bool      walkable;
};

// Maximum slope angle (degrees from vertical) that is considered walkable.
static constexpr float kMaxWalkableAngle = 45.f;

// Sample spacing in world units. Smaller = more accurate but larger file.
static constexpr float kSampleStep = 1.0f;

// Extract triangle candidates from a heightmap.
// Each grid cell produces 2 triangles; each is classified walkable by slope.
inline std::vector<NavTriCandidate> ExtractCandidates(const Heightmap& hm) {
    std::vector<NavTriCandidate> out;
    if (hm.W < 2 || hm.H < 2) return out;

    const float cs    = hm.cell_size;
    const float cosT  = std::cos(glm::radians(kMaxWalkableAngle));

    // Step in grid cells matching kSampleStep world units.
    int step = std::max(1, static_cast<int>(kSampleStep / cs + 0.5f));

    out.reserve(((hm.W - 1) / step) * ((hm.H - 1) / step) * 2);

    for (int iz = 0; iz + step < hm.H; iz += step) {
        for (int ix = 0; ix + step < hm.W; ix += step) {
            // World-space corners of this cell.
            float x0 = ix       * cs, x1 = (ix + step) * cs;
            float z0 = iz       * cs, z1 = (iz + step) * cs;

            glm::vec3 p00 = { x0, hm.SampleWorld(x0, z0), z0 };
            glm::vec3 p10 = { x1, hm.SampleWorld(x1, z0), z0 };
            glm::vec3 p01 = { x0, hm.SampleWorld(x0, z1), z1 };
            glm::vec3 p11 = { x1, hm.SampleWorld(x1, z1), z1 };

            // Lower-left triangle: p00, p10, p01
            {
                glm::vec3 n = glm::normalize(glm::cross(p10 - p00, p01 - p00));
                NavTriCandidate c;
                c.v[0] = p00; c.v[1] = p10; c.v[2] = p01;
                c.walkable = n.y >= cosT;
                out.push_back(c);
            }
            // Upper-right triangle: p10, p11, p01
            {
                glm::vec3 n = glm::normalize(glm::cross(p11 - p10, p01 - p10));
                NavTriCandidate c;
                c.v[0] = p10; c.v[1] = p11; c.v[2] = p01;
                c.walkable = n.y >= cosT;
                out.push_back(c);
            }
        }
    }
    return out;
}

} // namespace nav
