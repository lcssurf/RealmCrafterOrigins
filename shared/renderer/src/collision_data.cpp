#include "rco/renderer/collision_data.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <algorithm>

// Moved verbatim from client/src/renderer/terrain/terrain.cpp during the
// player-physics reformulation (see docs/TECH_DEBT.md #125's architecture
// investigation) — same code, new home, so shared/physics can reference
// ColBox/ColSphere/ColTri/ColData without depending on client/. See
// collision_data.h's file doc comment: LEGACY as of the physics
// reformulation (the player no longer calls Resolve()), left functional.

namespace rco::renderer {

// ---------------------------------------------------------------------------
// Collision data — load from coldata.bin
// ---------------------------------------------------------------------------

ColData LoadColData(const std::string& area_name) {
    char path[512];
    std::snprintf(path, sizeof(path), "data/areas/%s/coldata.bin", area_name.c_str());
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};

    auto r32 = [&](uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; };
    auto rf  = [&](float&    v) { return std::fread(&v, 4, 1, f) == 1; };

    uint32_t magic, version;
    if (!r32(magic) || magic != 0x444C4F43u) { std::fclose(f); return {}; }
    if (!r32(version) || version > 3)        { std::fclose(f); return {}; }

    // Precomputes rot (Ry*Rx*Rz, degrees — same convention as zone_scene's
    // scenery TRS) and the box's world-space Y bounds (via its 8 rotated
    // corners) once at load, so the per-frame early-out stays a cheap
    // min/max compare instead of re-deriving this every call.
    auto finalizeBox = [](ColBox& b, float pitchDeg, float yawDeg, float rollDeg) {
        glm::mat4 m(1.f);
        m = glm::rotate(m, glm::radians(yawDeg),  glm::vec3(0, 1, 0));
        m = glm::rotate(m, glm::radians(pitchDeg), glm::vec3(1, 0, 0));
        m = glm::rotate(m, glm::radians(rollDeg),  glm::vec3(0, 0, 1));
        b.rot = glm::mat3(m);
        float yMin = 1e30f, yMax = -1e30f;
        for (int i = 0; i < 8; ++i) {
            glm::vec3 local(
                (i & 1) ? b.half.x : -b.half.x,
                (i & 2) ? b.half.y : -b.half.y,
                (i & 4) ? b.half.z : -b.half.z);
            float wy = (b.rot * local).y + b.pos.y;
            yMin = std::min(yMin, wy);
            yMax = std::max(yMax, wy);
        }
        b.worldYMin = yMin;
        b.worldYMax = yMax;
    };

    ColData out;
    uint32_t n;
    if (!r32(n)) { std::fclose(f); return {}; }
    out.boxes.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        ColBox b;
        float pitchDeg = 0.f, yawDeg = 0.f, rollDeg = 0.f;
        if (!rf(b.pos.x)||!rf(b.pos.y)||!rf(b.pos.z)||
            !rf(b.half.x)||!rf(b.half.y)||!rf(b.half.z)) break;
        if (version >= 3) {
            if (!rf(pitchDeg)||!rf(yawDeg)||!rf(rollDeg)) break;
        }
        finalizeBox(b, pitchDeg, yawDeg, rollDeg);
        out.boxes.push_back(b);
    }
    if (!r32(n)) { std::fclose(f); out.loaded = true; return out; }
    out.spheres.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        ColSphere s;
        if (!rf(s.pos.x)||!rf(s.pos.y)||!rf(s.pos.z)||!rf(s.radius)) break;
        out.spheres.push_back(s);
    }
    if (version >= 2) {
        if (!r32(n)) { std::fclose(f); out.loaded = true; return out; }
        out.tris.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            ColTri t;
            bool ok = rf(t.v[0].x)&&rf(t.v[0].y)&&rf(t.v[0].z)
                    &&rf(t.v[1].x)&&rf(t.v[1].y)&&rf(t.v[1].z)
                    &&rf(t.v[2].x)&&rf(t.v[2].y)&&rf(t.v[2].z);
            if (!ok) break;
            out.tris.push_back(t);
        }
    }
    std::fclose(f);
    out.loaded = true;
    return out;
}

// ---------------------------------------------------------------------------
// Collision resolution — push player out of boxes and spheres
// ---------------------------------------------------------------------------

// Closest point on a 2D line segment to point p (Ericson).
static glm::vec2 ClosestPtSeg2D(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    glm::vec2 ab = b - a, ap = p - a;
    float len2 = glm::dot(ab, ab);
    if (len2 < 1e-10f) return a;
    float t = std::max(0.f, std::min(1.f, glm::dot(ap, ab) / len2));
    return a + t * ab;
}

// Closest point on 2D triangle (a,b,c) to point p — Ericson barycentric method.
// Handles degenerate (collinear) triangles gracefully.
static glm::vec2 ClosestPtTri2D(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
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
        // Degenerate triangle — fall back to closest edge
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

void ColData::Resolve(float& px, float pz_in, float py, float& out_pz,
                       const std::vector<ColBox>& dynamicBoxes) const {
    const float R = kPlayerCapsuleRadius;
    const float H = kPlayerCapsuleHeight;
    float pz = pz_in;
    float pYmin = py, pYmax = py + H;

    for (int iter = 0; iter < 3; ++iter) {
        // True OBB test: clamp the capsule's mid-height point in the box's
        // OWN local axes (rot may be a rotated frame, not just identity),
        // then bring the closest point back to world space. For an
        // axis-aligned box (rot=identity — every COLD v2 file, and every
        // dynamicBoxes entry, which deliberately ignores rotation),
        // transpose(rot) is the identity too, so this reduces to the exact
        // same clamp-in-world-space the old AABB-only test did.
        auto testBox = [&](const ColBox& b, bool isDynamicForLog) {
            // Throttled (~600ms), unconditional diagnostic for the
            // dynamic-collision investigation — logs every dynamicBoxes
            // test regardless of outcome, so we can see whether the Y-range
            // gate below is the thing silently rejecting the box (player
            // capsule at a different height than the box's worldYMin/Max)
            // or whether the box is in range but the X/Z overlap test still
            // isn't pushing the player out.
            static auto s_last_dyncol_testbox_log = std::chrono::steady_clock::time_point{};
            const bool doLog = isDynamicForLog && [] {
                auto nowT = std::chrono::steady_clock::now();
                if (nowT - s_last_dyncol_testbox_log >= std::chrono::milliseconds(600)) {
                    s_last_dyncol_testbox_log = nowT;
                    return true;
                }
                return false;
            }();

            if (pYmax < b.worldYMin || pYmin > b.worldYMax) {
                if (doLog) {
                    std::fprintf(stderr,
                        "[dyncol][testBox] Y-REJECT player pos=(%.3f,%.3f,%.3f) "
                        "capsule pYmin=%.3f pYmax=%.3f  box center=(%.3f,%.3f,%.3f) "
                        "half=(%.3f,%.3f,%.3f) worldYMin=%.3f worldYMax=%.3f "
                        "-> horizontal test SKIPPED, no vertical overlap\n",
                        px, py, pz, pYmin, pYmax,
                        b.pos.x, b.pos.y, b.pos.z, b.half.x, b.half.y, b.half.z,
                        b.worldYMin, b.worldYMax);
                }
                return;
            }
            // Step-up bypass: a box whose top is within kMaxStepHeight of
            // the player's CURRENT feet (pYmin) is a climbable
            // curb/stair/low-ledge, not a wall — skip the horizontal push
            // entirely.
            if (b.worldYMax - pYmin <= kMaxStepHeight) {
                if (doLog) {
                    std::fprintf(stderr,
                        "[dyncol][testBox] STEP-UP player pos=(%.3f,%.3f,%.3f) "
                        "worldYMax=%.3f pYmin=%.3f diff=%.3f <= kMaxStepHeight=%.3f "
                        "-> horizontal push SKIPPED, treated as climbable\n",
                        px, py, pz, b.worldYMax, pYmin, b.worldYMax - pYmin, kMaxStepHeight);
                }
                return;
            }
            glm::vec3 worldPt(px, (pYmin + pYmax) * 0.5f, pz);
            glm::vec3 local = glm::transpose(b.rot) * (worldPt - b.pos);
            local = glm::clamp(local, -b.half, b.half);
            glm::vec3 closest = b.pos + b.rot * local;
            float dx = px - closest.x, dz = pz - closest.z;
            float d2 = dx*dx + dz*dz;
            const bool collided = d2 < R * R;
            if (collided) {
                if (d2 < 1e-7f) { dx = 1.f; dz = 0.f; d2 = 1.f; }
                float d = std::sqrt(d2);
                float push = R - d;
                px += (dx / d) * push;
                pz += (dz / d) * push;
            }
            if (doLog) {
                std::fprintf(stderr,
                    "[dyncol][testBox] Y-OK player pos=(%.3f,%.3f,%.3f) closest=(%.3f,%.3f,%.3f) "
                    "dist2=%.5f R2=%.5f -> %s\n",
                    px, py, pz, closest.x, closest.y, closest.z, d2, R * R,
                    collided ? "COLLIDED (pushed out)" : "no horizontal overlap (dist2 >= R^2)");
            }
        };
        // Static (coldata.bin) boxes AND per-frame dynamic boxes, both
        // inside this same iteration — not a separate pass after the loop.
        for (const auto& b : boxes) testBox(b, false);
        for (const auto& b : dynamicBoxes) testBox(b, true);
        for (const auto& s : spheres) {
            if (pYmax < s.pos.y - s.radius || pYmin > s.pos.y + s.radius) continue;
            float dx = px - s.pos.x, dz = pz - s.pos.z;
            float d2 = dx*dx + dz*dz;
            float minD = R + s.radius;
            if (d2 < minD * minD) {
                if (d2 < 1e-7f) { dx = 1.f; dz = 0.f; d2 = 1.f; }
                float d = std::sqrt(d2);
                px += (dx / d) * (minD - d);
                pz += (dz / d) * (minD - d);
            }
        }
        for (const auto& tri : tris) {
            float tYmin = std::min({tri.v[0].y, tri.v[1].y, tri.v[2].y});
            float tYmax = std::max({tri.v[0].y, tri.v[1].y, tri.v[2].y});
            if (pYmax < tYmin || pYmin > tYmax) continue;
            glm::vec2 a{tri.v[0].x, tri.v[0].z};
            glm::vec2 b{tri.v[1].x, tri.v[1].z};
            glm::vec2 c{tri.v[2].x, tri.v[2].z};
            glm::vec2 pp{px, pz};
            glm::vec2 closest = ClosestPtTri2D(pp, a, b, c);
            glm::vec2 diff = pp - closest;
            float d2 = glm::dot(diff, diff);
            if (d2 < R * R) {
                if (d2 < 1e-7f) { diff = {1.f, 0.f}; d2 = 1.f; }
                float d = std::sqrt(d2);
                px += (diff.x / d) * (R - d);
                pz += (diff.y / d) * (R - d);
            }
        }
    }
    out_pz = pz;
}

} // namespace rco::renderer
