#include "rco/physics/capsule_sweep.h"
#include <algorithm>
#include <cmath>

namespace rco::physics {

using renderer::ColBox;
using renderer::ColSphere;
using renderer::ColTri;

namespace {

// Ray-vs-slab test on one axis: intersects [p, p+d] against [mn, mx],
// returns the entry/exit t (may be outside [0,1] — caller clips). Returns
// false only when the ray is parallel to the slab AND starts outside it
// (never intersects, at any t).
bool SlabAxis(float p, float d, float mn, float mx, float& tEnter, float& tExit) {
    if (std::abs(d) < 1e-8f) {
        if (p < mn || p > mx) { tEnter = 1e30f; tExit = -1e30f; return false; }
        tEnter = -1e30f; tExit = 1e30f;
        return true;
    }
    float t1 = (mn - p) / d, t2 = (mx - p) / d;
    if (t1 > t2) std::swap(t1, t2);
    tEnter = t1; tExit = t2;
    return true;
}

// Closest point on a 2D line segment to point p (Ericson) — same technique
// as the legacy ClosestPtTri2D's degenerate-triangle fallback.
glm::vec2 ClosestPtSeg2D(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    glm::vec2 ab = b - a, ap = p - a;
    float len2 = glm::dot(ab, ab);
    if (len2 < 1e-10f) return a;
    float t = std::max(0.f, std::min(1.f, glm::dot(ap, ab) / len2));
    return a + t * ab;
}

// Closest point on 2D triangle (a,b,c) to point p — Ericson barycentric
// method, same algorithm as the legacy renderer collision code
// (terrain.cpp's now-superseded ClosestPtTri2D), duplicated here since
// shared/physics can't call back into client code.
glm::vec2 ClosestPtTri2D(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    glm::vec2 ab = b-a, ac = c-a, ap = p-a;
    float d1 = glm::dot(ab,ap), d2 = glm::dot(ac,ap);
    if (d1 <= 0.f && d2 <= 0.f) return a;
    glm::vec2 bp = p-b;
    float d3 = glm::dot(ab,bp), d4 = glm::dot(ac,bp);
    if (d3 >= 0.f && d4 <= d3) return b;
    float vc = d1*d4 - d3*d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
        float v = d1/(d1-d3); return a + v*ab;
    }
    glm::vec2 cp = p-c;
    float d5 = glm::dot(ab,cp), d6 = glm::dot(ac,cp);
    if (d6 >= 0.f && d5 <= d6) return c;
    float vb = d5*d2 - d1*d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
        float w = d2/(d2-d6); return a + w*ac;
    }
    float va = d3*d6 - d5*d4;
    if (va <= 0.f && (d4-d3) >= 0.f && (d5-d6) >= 0.f) {
        float w = (d4-d3)/((d4-d3)+(d5-d6)); return b + w*(c-b);
    }
    float denom = va+vb+vc;
    if (std::abs(denom) < 1e-10f) {
        glm::vec2 p1 = ClosestPtSeg2D(p,a,b);
        glm::vec2 p2 = ClosestPtSeg2D(p,b,c);
        glm::vec2 p3 = ClosestPtSeg2D(p,c,a);
        float l1=glm::length(p-p1), l2=glm::length(p-p2), l3=glm::length(p-p3);
        if (l1<=l2&&l1<=l3) return p1;
        return l2<=l3 ? p2 : p3;
    }
    float inv = 1.f/denom, v = vb*inv, w = vc*inv;
    return a + v*ab + w*ac;
}

// Barycentric containment + height interpolation for one triangle at world
// XZ (x,z). Returns false when (x,z) is outside the triangle's projection.
// Shared by RayVerticalHitsTri (ground probe) and SweepCapsuleVsTriangle's
// "am I standing on this triangle's actual local surface" floor-contact
// check — both need the EXACT interpolated height at a point, which for a
// sloped/arched triangle can differ meaningfully from any single vertex's Y
// (why this exists separately from ClosestPtTri2D, which is boundary-
// oriented and doesn't return a height at all).
bool TriBarycentricHeight(float x, float z, const ColTri& tri, float& outY) {
    const glm::vec2 a{tri.v[0].x, tri.v[0].z};
    const glm::vec2 b{tri.v[1].x, tri.v[1].z};
    const glm::vec2 c{tri.v[2].x, tri.v[2].z};
    const float minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
    if (x < minX || x > maxX) return false;
    const float minZ = std::min({a.y, b.y, c.y}), maxZ = std::max({a.y, b.y, c.y});
    if (z < minZ || z > maxZ) return false;

    const glm::vec2 p{x, z};
    const glm::vec2 v0 = b - a, v1 = c - a, v2 = p - a;
    const float d00 = glm::dot(v0, v0), d01 = glm::dot(v0, v1), d11 = glm::dot(v1, v1);
    const float d20 = glm::dot(v2, v0), d21 = glm::dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (std::abs(denom) < 1e-8f) return false;

    const float invDenom = 1.f / denom;
    const float v = (d11 * d20 - d01 * d21) * invDenom;
    const float w = (d00 * d21 - d01 * d20) * invDenom;
    const float u = 1.f - v - w;
    constexpr float kEdgeEps = -1e-4f;
    if (u < kEdgeEps || v < kEdgeEps || w < kEdgeEps) return false;

    outY = u * tri.v[0].y + v * tri.v[1].y + w * tri.v[2].y;
    return true;
}

// Below this margin, "obstacle top at/above capsule feet" is treated as
// standing ON the obstacle (a floor/support contact) rather than colliding
// INTO it (a wall contact) — see SweepCapsuleVsBox/Triangle's Y-band gates
// and their doc comments in capsule_sweep.h for the bug this fixes (a
// player couldn't walk while standing on a Box/Mesh surface: the same
// geometry holding them up was ALSO being swept as a horizontal wall,
// since their feet sat exactly at/on its top).
constexpr float kFloorContactEpsilon = 0.02f;

} // namespace

SweepHit SweepCapsuleVsBox(const glm::vec3& pos, float radius, float height,
                            const glm::vec3& delta, const ColBox& box) {
    SweepHit result;

    glm::vec3 localPos   = glm::transpose(box.rot) * (pos - box.pos);
    glm::vec3 localDelta = glm::transpose(box.rot) * delta;

    // Minkowski sum: inflate the box's XZ half-extents by the capsule
    // radius (see header doc — exact on flat faces, approximate at corners).
    float ex = box.half.x + radius;
    float ez = box.half.z + radius;

    float tEnterX, tExitX, tEnterZ, tExitZ;
    bool validX = SlabAxis(localPos.x, localDelta.x, -ex, ex, tEnterX, tExitX);
    bool validZ = SlabAxis(localPos.z, localDelta.z, -ez, ez, tEnterZ, tExitZ);
    if (!validX || !validZ) return result;

    float tEnter = std::max(tEnterX, tEnterZ);
    float tExit  = std::min(tExitX, tExitZ);
    if (tEnter > tExit || tEnter > 1.f || tExit < 0.f) return result;

    float t = std::max(tEnter, 0.f); // already overlapping at start -> report t=0

    // Y-band check at the moment of XZ contact: does the capsule's vertical
    // extent overlap the box's world Y bounds there? (worldYMin/YMax are
    // already world-space, no local-frame conversion needed.)
    //
    // capsuleYmin >= box.worldYMax - kFloorContactEpsilon (not a strict >)
    // is deliberate: a capsule whose FEET are at/above the box's flat top
    // is standing ON it, not colliding into its side — that's a floor
    // contact for the ground probe to own, never a horizontal wall. Without
    // this margin, a player resting exactly at worldYMax (the normal case
    // right after ProbeGround snaps them there) would have their own
    // support box register as a "wall" the instant they tried to walk,
    // since the box's inflated-by-radius XZ footprint still contains them.
    float capsuleYmin = pos.y + t * delta.y;
    float capsuleYmax = capsuleYmin + height;
    if (capsuleYmax < box.worldYMin || capsuleYmin >= box.worldYMax - kFloorContactEpsilon) {
        return result; // passes over/under the box, or is standing on its top
    }

    glm::vec3 localNormal = (tEnterX >= tEnterZ)
        ? glm::vec3((localDelta.x < 0.f) ? 1.f : -1.f, 0.f, 0.f)
        : glm::vec3(0.f, 0.f, (localDelta.z < 0.f) ? 1.f : -1.f);

    result.hit = true;
    result.t = t;
    result.normal = glm::normalize(box.rot * localNormal);
    result.point = pos + t * delta;
    result.surface_top_y = box.worldYMax;
    return result;
}

SweepHit SweepCapsuleVsTriangle(const glm::vec3& pos, float radius, float height,
                                 const glm::vec3& delta, const ColTri& tri) {
    SweepHit result;

    const float tYmin = std::min({tri.v[0].y, tri.v[1].y, tri.v[2].y});
    const float tYmax = std::max({tri.v[0].y, tri.v[1].y, tri.v[2].y});
    const glm::vec2 a{tri.v[0].x, tri.v[0].z};
    const glm::vec2 b{tri.v[1].x, tri.v[1].z};
    const glm::vec2 c{tri.v[2].x, tri.v[2].z};

    // Uses the EXACT interpolated surface height at each query point
    // (TriBarycentricHeight), not the triangle's raw max vertex — critical
    // for sloped/arched geometry (a curved bridge deck): the tri's tallest
    // vertex can sit well above where the capsule's feet actually rest at
    // THIS xz, so a coarse "feet above tYmax" check would miss most
    // standing-on-top contacts on a slope and keep wrongly treating them as
    // walls. See kFloorContactEpsilon's doc comment for why this matters.
    auto TestAt = [&](float t, glm::vec2& outClosest) -> bool {
        glm::vec3 p = pos + t * delta;
        float pYmin = p.y, pYmax = p.y + height;
        if (pYmax < tYmin || pYmin > tYmax) return false;

        float surfY;
        if (TriBarycentricHeight(p.x, p.z, tri, surfY) && pYmin >= surfY - kFloorContactEpsilon) {
            return false; // standing on this triangle's local surface here — floor, not a wall
        }

        glm::vec2 pp{p.x, p.z};
        outClosest = ClosestPtTri2D(pp, a, b, c);
        glm::vec2 diff = pp - outClosest;
        return glm::dot(diff, diff) <= radius * radius;
    };

    glm::vec2 closest0;
    if (TestAt(0.f, closest0)) {
        glm::vec2 diff = glm::vec2(pos.x, pos.z) - closest0;
        float len = glm::length(diff);
        result.hit = true;
        result.t = 0.f;
        result.normal = (len > 1e-6f) ? glm::vec3(diff.x/len, 0.f, diff.y/len)
                                       : glm::vec3(1.f, 0.f, 0.f);
        result.point = pos;
        result.surface_top_y = tYmax;
        return result;
    }

    constexpr int kSubsteps = 8;
    float prevT = 0.f;
    for (int i = 1; i <= kSubsteps; ++i) {
        float t = float(i) / float(kSubsteps);
        glm::vec2 closest;
        if (TestAt(t, closest)) {
            // Bisect the miss->hit interval a few times for a tighter t.
            float lo = prevT, hi = t;
            for (int iter = 0; iter < 4; ++iter) {
                float mid = (lo + hi) * 0.5f;
                glm::vec2 cp;
                if (TestAt(mid, cp)) hi = mid; else lo = mid;
            }
            glm::vec2 cp;
            TestAt(hi, cp);
            glm::vec3 pHit = pos + hi * delta;
            glm::vec2 diff = glm::vec2(pHit.x, pHit.z) - cp;
            float len = glm::length(diff);
            result.hit = true;
            result.t = hi;
            result.normal = (len > 1e-6f) ? glm::vec3(diff.x/len, 0.f, diff.y/len)
                                           : glm::vec3(0.f, 0.f, 1.f);
            result.point = pHit;
            result.surface_top_y = tYmax;
            return result;
        }
        prevT = t;
    }
    return result;
}

SweepHit SweepCapsuleVsSphere(const glm::vec3& pos, float radius, float height,
                               const glm::vec3& delta, const ColSphere& sphere) {
    SweepHit result;

    float pYmin0 = pos.y, pYmax0 = pos.y + height;
    float pYmin1 = pos.y + delta.y, pYmax1 = pYmin1 + height;
    bool bandOverlap0 = !(pYmax0 < sphere.pos.y - sphere.radius || pYmin0 > sphere.pos.y + sphere.radius);
    bool bandOverlap1 = !(pYmax1 < sphere.pos.y - sphere.radius || pYmin1 > sphere.pos.y + sphere.radius);
    if (!bandOverlap0 && !bandOverlap1) return result;

    glm::vec2 p0{pos.x, pos.z}, d{delta.x, delta.z}, c{sphere.pos.x, sphere.pos.z};
    glm::vec2 f = p0 - c;
    float combinedR = radius + sphere.radius;
    float a = glm::dot(d, d);
    float b = 2.f * glm::dot(f, d);
    float cc = glm::dot(f, f) - combinedR * combinedR;

    float t = -1.f;
    if (a < 1e-8f) {
        if (cc <= 0.f) t = 0.f; // stationary and already overlapping
    } else {
        float disc = b*b - 4.f*a*cc;
        if (disc >= 0.f) {
            float sq = std::sqrt(disc);
            float t0 = (-b - sq) / (2.f*a);
            float t1 = (-b + sq) / (2.f*a);
            if (t0 >= 0.f) t = t0;
            else if (t1 >= 0.f) t = 0.f; // already overlapping at start
        }
    }
    if (t < 0.f || t > 1.f) return result;

    glm::vec2 hitPos = p0 + t*d;
    glm::vec2 diff = hitPos - c;
    float len = glm::length(diff);
    result.hit = true;
    result.t = t;
    result.normal = (len > 1e-6f) ? glm::vec3(diff.x/len, 0.f, diff.y/len)
                                   : glm::vec3(1.f, 0.f, 0.f);
    result.point = pos + t * delta;
    return result;
}

bool RayVerticalHitsTri(float x, float z, float y_start,
                         const ColTri& tri, float& outY) {
    float y;
    if (!TriBarycentricHeight(x, z, tri, y)) return false;
    if (y > y_start) return false;
    outY = y;
    return true;
}

} // namespace rco::physics
