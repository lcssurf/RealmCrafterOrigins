#include "renderer/terrain/terrain.h"
#include "rco/renderer/pipeline.h"
#include <stb_image.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <chrono>

namespace rco::renderer {

// True when the player's CAPSULE (a circle of the given radius around
// (x, z), not just its exact center point) overlaps the box's XZ footprint
// (rotation undone via its precomputed local frame). Probes at the box's
// own center height, which is exact for yaw-only rotation (see
// ComputeGroundHeight's doc comment) and an approximation otherwise.
//
// Radius-aware on purpose (TECH_DEBT.md #125): a strict center-point-only
// test rejects the player the instant their exact feet-center crosses the
// footprint edge, even though their body — a capsule of radius R, not a
// point — still visibly overlaps the object right up to the edge. That
// produced the "pulled to the ground right at the edge/corner" symptom for
// BOTH Box/Wedge and Mesh ground candidates, since this is the ONE shared
// function tryBox (below) calls for all three kinds. Uses the same
// clamp-to-box + compare-distance-to-R technique already validated by
// Resolve()'s testBox horizontal push (terrain.cpp) — not a new formula,
// just applied here too.
//
// outLocalX/outLocalZ (optional): the raw LOCAL-frame coordinates before
// the radius/half-extent comparison, purely for diagnostics — lets a
// caller log the actual margin (or overshoot) against b.half instead of
// only the pass/fail boolean.
static bool PointInBoxFootprint(float x, float z, const ColBox& b, float radius,
                                 float* outLocalX = nullptr, float* outLocalZ = nullptr) {
    glm::vec3 local = glm::transpose(b.rot) * (glm::vec3(x, b.pos.y, z) - b.pos);
    if (outLocalX) *outLocalX = local.x;
    if (outLocalZ) *outLocalZ = local.z;
    glm::vec2 clampedXZ = glm::clamp(glm::vec2(local.x, local.z),
                                      glm::vec2(-b.half.x, -b.half.z),
                                      glm::vec2(b.half.x, b.half.z));
    float dx = local.x - clampedXZ.x;
    float dz = local.z - clampedXZ.y;
    return (dx * dx + dz * dz) <= radius * radius;
}

// Ray-plane intersection for one triangle: does the vertical column at
// (x, z) pass through tri's XZ projection, and if so, is the resulting
// surface height at or below y_start (i.e. would a ray cast straight down
// from y_start actually reach this triangle, not pass over it)?
//
// Containment test is a standard 2D barycentric-coordinate check on the
// triangle's (x, z) projection — cheap and exact for arbitrary (non-
// degenerate) triangles, including steep/near-vertical ones as long as
// they still have SOME XZ-projected area (a perfectly vertical wall
// triangle has zero XZ area and correctly never matches here, which is
// right: a vertical wall isn't something you stand ON). Height at the hit
// point is the same barycentric weights applied to the triangle's Y
// values — exact for the triangle's own plane, no interpolation error,
// unlike sampling a regular heightmap grid.
static bool RayVerticalHitsTri(float x, float z, float y_start,
                                const ColTri& tri, float& outY) {
    const glm::vec2 a{tri.v[0].x, tri.v[0].z};
    const glm::vec2 b{tri.v[1].x, tri.v[1].z};
    const glm::vec2 c{tri.v[2].x, tri.v[2].z};

    // Cheap XZ bounding-box reject before the full barycentric math —
    // col_data.tris has no per-object grouping (see TECH_DEBT.md #125's
    // cost analysis: 28k+ triangles for a single area, flattened, no
    // spatial index), so this is the only low-cost filter available
    // without adding a new spatial structure. Most triangles reject here
    // on the very first comparison.
    const float minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
    if (x < minX || x > maxX) return false;
    const float minZ = std::min({a.y, b.y, c.y}), maxZ = std::max({a.y, b.y, c.y});
    if (z < minZ || z > maxZ) return false;

    const glm::vec2 p{x, z};
    const glm::vec2 v0 = b - a, v1 = c - a, v2 = p - a;
    const float d00 = glm::dot(v0, v0), d01 = glm::dot(v0, v1), d11 = glm::dot(v1, v1);
    const float d20 = glm::dot(v2, v0), d21 = glm::dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (std::abs(denom) < 1e-8f) return false; // zero XZ area (near-vertical triangle) — not standable

    const float invDenom = 1.f / denom;
    const float v = (d11 * d20 - d01 * d21) * invDenom;
    const float w = (d00 * d21 - d01 * d20) * invDenom;
    const float u = 1.f - v - w;
    constexpr float kEdgeEps = -1e-4f; // small tolerance so points exactly on an edge still hit
    if (u < kEdgeEps || v < kEdgeEps || w < kEdgeEps) return false;

    const float y = u * tri.v[0].y + v * tri.v[1].y + w * tri.v[2].y;
    if (y > y_start) return false; // this triangle's surface is above the ray's start — not reachable downward
    outY = y;
    return true;
}

std::optional<float> SampleMeshGroundHeight(float x, float z, float y_start,
                                             const std::vector<ColTri>& tris) {
    std::optional<float> best;
    for (const auto& tri : tris) {
        float y;
        if (RayVerticalHitsTri(x, z, y_start, tri, y)) {
            // Highest hit at/below y_start wins — same as a real ray
            // stopping at the FIRST surface it reaches coming down, so a
            // bridge deck is picked over a pillar underneath it, but a
            // pillar top is still picked correctly when standing under the
            // deck (y_start below the deck's height to begin with).
            if (!best || y > *best) best = y;
        }
    }
    return best;
}

float ComputeGroundHeight(float x, float z, float support_ceiling_y,
                           const Terrain& terrain, const ColData& col_data,
                           const std::vector<ColBox>& dynamicBoxes) {
    float ground = terrain.SampleHeight(x, z);
    const char* source = "terrain";
    auto tryBox = [&](const ColBox& b, const char* kind) {
        const bool overCeiling = b.worldYMax > support_ceiling_y;
        const bool notHigher   = !overCeiling && b.worldYMax <= ground;
        if (overCeiling || notHigher) return;
        if (PointInBoxFootprint(x, z, b, kPlayerCapsuleRadius)) {
            ground = b.worldYMax;
            source = kind;
        }
    };
    for (const auto& b : col_data.boxes)    tryBox(b, "box");
    for (const auto& b : dynamicBoxes)      tryBox(b, "dynamicBox");

    // Mesh candidate: real vertical raycast against col_data.tris (the same
    // triangles ColData::Resolve() already tests for horizontal push — no
    // new data) instead of the retired AABB/center approximation
    // (TECH_DEBT.md #125). Exact for arbitrary geometry — arches, ramps,
    // anything — since it's a real ray-plane intersection per triangle, not
    // a stand-in shape.
    std::optional<float> meshY = SampleMeshGroundHeight(x, z, support_ceiling_y, col_data.tris);
    if (meshY.has_value() && *meshY > ground) {
        ground = *meshY;
        source = "mesh";
    }
    (void)source; // kept for readability/future debugging, not otherwise read
    return ground;
}

// ---------------------------------------------------------------------------
// Procedural fallback height
// ---------------------------------------------------------------------------
static float ProceduralHeight(float x, float z) {
    float h = 0.f;
    h += std::sin(x * 0.05f) * std::cos(z * 0.04f) * 4.f;
    h += std::sin(x * 0.12f + 1.3f) * std::cos(z * 0.11f) * 2.f;
    h += std::sin(x * 0.31f) * std::cos(z * 0.28f + 0.7f) * 0.8f;
    h += std::sin(x * 0.70f + 2.1f) * std::cos(z * 0.65f) * 0.3f;
    return h;
}

// ---------------------------------------------------------------------------
// Texture helpers
// ---------------------------------------------------------------------------
GLuint Terrain::MakeSolidTex(uint8_t r, uint8_t g, uint8_t b) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    const uint8_t px[4] = {r, g, b, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

static GLuint LoadTexInternal(const std::string& path, bool srgb) {
    int w, h, ch;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) return 0;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLenum internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    stbi_image_free(px);
    return tex;
}

GLuint Terrain::LoadSRGBTex(const std::string& path)   { return LoadTexInternal(path, true); }
GLuint Terrain::LoadLinearTex(const std::string& path) { return LoadTexInternal(path, false); }

// ---------------------------------------------------------------------------
// Texture role detection (mirrors terrain-editor material.h)
// ---------------------------------------------------------------------------
enum class TexRole { Albedo, Normal, Roughness, AO, Height, ORM, Unknown };

static TexRole GuessRole(const std::string& stem) {
    std::string s = stem;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);

    // Normal must be tested BEFORE ORM/ARM, otherwise "_normal-ogl" matches
    // the "orm" substring inside "n[orm]al" and gets miscategorised.
    bool isDX = s.find("_dx") != std::string::npos || s.find("dx_") != std::string::npos;
    if (!isDX && (s.find("nor") != std::string::npos ||
                  s.find("nrm") != std::string::npos ||
                  s.find("normal") != std::string::npos))
        return TexRole::Normal;

    // ORM/ARM must be matched on word-ish boundaries to avoid the "normal"
    // collision above. We require a separator (_/-/.) before or after.
    auto isMatchedToken = [&](const std::string& tok) {
        size_t pos = 0;
        while ((pos = s.find(tok, pos)) != std::string::npos) {
            bool leftOk  = (pos == 0) || s[pos-1] == '_' || s[pos-1] == '-' || s[pos-1] == '.';
            size_t end   = pos + tok.size();
            bool rightOk = (end == s.size()) || s[end] == '_' || s[end] == '-' || s[end] == '.';
            if (leftOk && rightOk) return true;
            pos = end;
        }
        return false;
    };
    if (isMatchedToken("orm") || isMatchedToken("arm"))
        return TexRole::ORM;
    if (s.find("col") != std::string::npos || s.find("albedo") != std::string::npos ||
        s.find("diffuse") != std::string::npos || s.find("diff") != std::string::npos ||
        s.find("basecolor") != std::string::npos || s.find("base_color") != std::string::npos)
        return TexRole::Albedo;
    if (s.find("rough") != std::string::npos) return TexRole::Roughness;
    if (s == "ao" || s.find("_ao") != std::string::npos || s.find("ao_") != std::string::npos ||
        s.find("ambient") != std::string::npos || s.find("occlusion") != std::string::npos)
        return TexRole::AO;
    if (s.find("height") != std::string::npos || s.find("disp") != std::string::npos ||
        s.find("bump") != std::string::npos)
        return TexRole::Height;
    return TexRole::Unknown;
}

static bool IsImg(const std::filesystem::path& p) {
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga";
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool Terrain::Init(int gw, int gh) {
    grid_w_ = gw;
    grid_h_ = gh;

    def_normal_    = MakeSolidTex(128, 128, 255);
    def_roughness_ = MakeSolidTex(180, 180, 180);
    def_ao_        = MakeSolidTex(255, 255, 255);   // no occlusion
    def_height_    = MakeSolidTex(128, 128, 128);   // mid-height (neutral for height-blend)
    def_macro_     = MakeSolidTex(128, 128, 128);   // 0.5 gray = no overlay change

    GenerateProcedural();
    return true;
}

// ---------------------------------------------------------------------------
// LoadFromEditor
// ---------------------------------------------------------------------------
bool Terrain::LoadFromEditor(const std::string& area_name) {
    namespace fs = std::filesystem;

    std::string base = "data/areas/" + area_name + "/";
    std::string hmap_path  = base + "heightmap.bin";
    std::string smap_path  = base + "splatmap.bin";
    std::string mats_path  = base + "materials.txt";

    std::fprintf(stderr, "[terrain] LoadFromEditor: '%s' -> %s\n",
                 area_name.c_str(), hmap_path.c_str());

    // --- Load heightmap ---
    {
        std::ifstream f(hmap_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[terrain] heightmap not found: %s\n", hmap_path.c_str());
            return false;
        }

        uint32_t magic = 0;
        f.read(reinterpret_cast<char*>(&magic), 4);
        if (magic != 0x4D484352) {
            std::fprintf(stderr, "[terrain] bad magic in %s\n", hmap_path.c_str());
            return false;
        }

        int lw, lh;
        float cs;
        f.read(reinterpret_cast<char*>(&lw), 4);
        f.read(reinterpret_cast<char*>(&lh), 4);
        f.read(reinterpret_cast<char*>(&cs), 4);

        std::vector<float> tmp(lw * lh);
        f.read(reinterpret_cast<char*>(tmp.data()), tmp.size() * 4);
        if (!f) return false;

        hmap_data_    = std::move(tmp);
        hmap_w_       = lw;
        hmap_h_       = lh;
        hmap_cell_    = cs;
        has_hmap_     = true;
        std::fprintf(stderr, "[terrain] heightmap loaded: %dx%d cells, cell=%.1f\n", lw, lh, cs);

        // Each chunk covers (kSize-1) cells, so N chunks need N*(kSize-1)+1 verts.
        const int stride = TerrainChunk::kSize - 1;
        int cw = std::max(1, (lw - 1) / stride);
        int ch = std::max(1, (lh - 1) / stride);
        grid_w_ = cw;
        grid_h_ = ch;

        // World coords are 0-indexed to match the GUE Zone editor and the
        // npc_spawns / scenery tables (which store positions in [0, W*cs]).
        // Previously this was centered around the origin, causing every
        // placed object to render off-map on the client.
        hmap_origin_x_ = 0.f;
        hmap_origin_z_ = 0.f;
        hmap_size_x_   = (lw - 1) * cs;
        hmap_size_z_   = (lh - 1) * cs;
    }

    // Upload heightmap as R32F texture (read by vertex shader)
    {
        if (!hmap_tex_) glGenTextures(1, &hmap_tex_);
        glBindTexture(GL_TEXTURE_2D, hmap_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, hmap_w_, hmap_h_, 0,
                     GL_RED, GL_FLOAT, hmap_data_.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Rebuild chunks (static XZ grid — no heights in VBO)
    RebuildChunksFromHmap();

    // --- Load splatmap ---
    uint32_t dbg_smap_magic = 0;
    int      dbg_smap_layers = 0;
    {
        std::ifstream f(smap_path, std::ios::binary);
        if (f) {
            uint32_t magic = 0;
            f.read(reinterpret_cast<char*>(&magic), 4);
            dbg_smap_magic = magic;
            int sw, sh;
            f.read(reinterpret_cast<char*>(&sw), 4);
            f.read(reinterpret_cast<char*>(&sh), 4);

            std::vector<uint8_t> sdata;
            bool ok = false;

            splatmap_layers_.clear();
            splatmap_w_ = sw;
            splatmap_h_ = sh;

            if (magic == 0x4E505352) {
                // RSPN — GUE Phase 1 multi-layer format (magic|W|H|numLayers|
                // layer0 data|layer1 data|...). ALL layers are read now (not
                // just layer 0): layer 0 still feeds the legacy splatmap_tex_
                // (used when materials_.size() <= 4), and every layer feeds
                // splatmap_array_ (used when materials_.size() > 4, see
                // RebuildMaterialArrays/Submit). See docs/TECH_DEBT.md
                // "Terrain multi-material authoring (Phase 1)".
                int numLayers = 0;
                f.read(reinterpret_cast<char*>(&numLayers), 4);
                if (f && numLayers > 0) {
                    splatmap_layers_.reserve(numLayers);
                    bool allOk = true;
                    for (int li = 0; li < numLayers; ++li) {
                        std::vector<uint8_t> layer((size_t)sw * sh * 4);
                        f.read(reinterpret_cast<char*>(layer.data()), (std::streamsize)layer.size());
                        if (!f) { allOk = false; break; }
                        splatmap_layers_.push_back(std::move(layer));
                    }
                    if (allOk && !splatmap_layers_.empty()) {
                        sdata = splatmap_layers_[0]; // layer 0 -> legacy splatmap_tex_ below
                        ok = true;
                    }
                }
            } else if (magic == 0x32505352) {
                // RSP2 — native uint8 format (single layer, materials 0-3 only)
                sdata.resize((size_t)sw * sh * 4);
                f.read(reinterpret_cast<char*>(sdata.data()), (std::streamsize)sdata.size());
                ok = (bool)f;
                if (ok) splatmap_layers_.push_back(sdata);
            } else if (magic == 0x4D505352) {
                // RSPM — legacy float format; convert on load (single layer)
                std::vector<float> ftmp((size_t)sw * sh * 4);
                f.read(reinterpret_cast<char*>(ftmp.data()), (std::streamsize)(ftmp.size() * 4));
                if (f) {
                    sdata.resize(ftmp.size());
                    for (size_t i = 0; i < ftmp.size(); i++)
                        sdata[i] = static_cast<uint8_t>(std::clamp(ftmp[i] * 255.f + 0.5f, 0.f, 255.f));
                    ok = true;
                    splatmap_layers_.push_back(sdata);
                }
            }
            dbg_smap_layers = (int)splatmap_layers_.size();

            if (ok) {
                if (splatmap_tex_ == 0) glGenTextures(1, &splatmap_tex_);
                glBindTexture(GL_TEXTURE_2D, splatmap_tex_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, sdata.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        }
    }

    // --- Load macro variation ---
    {
        std::string macro_path = base + "macro.png";
        int mw, mh, mch;
        stbi_set_flip_vertically_on_load(false);
        unsigned char* px = stbi_load(macro_path.c_str(), &mw, &mh, &mch, 1);
        if (px) {
            if (macro_tex_) glDeleteTextures(1, &macro_tex_);
            glGenTextures(1, &macro_tex_);
            glBindTexture(GL_TEXTURE_2D, macro_tex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, mw, mh, 0, GL_RED, GL_UNSIGNED_BYTE, px);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            macro_strength_ = 0.3f;
            stbi_image_free(px);
            std::fprintf(stderr, "[terrain] macro variation loaded: %dx%d\n", mw, mh);
        }
    }

    // --- Load materials ---
    UnloadMaterials();
    {
        std::ifstream f(mats_path);
        if (f) {
            std::string line;
            // Phase 1: no longer capped at 4 — reads however many lines the
            // GUE wrote. <=4 materials still take the legacy exact-4-slot
            // shader path (see Submit()); >4 uses the generalized N-material
            // path via the texture arrays built below. See docs/TECH_DEBT.md
            // "Terrain multi-material authoring (Phase 1)".
            while (std::getline(f, line)) {
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if (line.empty()) continue;

                // Two formats:
                //   Old: "foldername [tiling]"   — first token is a non-numeric name
                //   New: "id tiling albedo normal orm"  — first token is a digit (media_materials id)
                std::istringstream is(line);
                std::string first;
                float tiling = 4.f;
                if (!(is >> first)) continue;
                is >> tiling;

                bool isNewFormat = !first.empty() && std::isdigit((unsigned char)first[0]);

                if (isNewFormat) {
                    // Extended format written by GUE — direct texture paths relative to dist/client/
                    // tokens: albedo normal orm normal_strength
                    std::string albedo_rel, normal_rel, orm_rel, ns_str;
                    is >> albedo_rel >> normal_rel >> orm_rel >> ns_str;

                    MatTex m;
                    m.tiling          = tiling;
                    m.normal_strength = ns_str.empty() ? 2.5f : [&]{
                        try { return std::stof(ns_str); } catch (...) { return 2.5f; }
                    }();
                    m.normal    = def_normal_;
                    m.roughness = def_roughness_;
                    m.ao        = def_ao_;
                    m.height    = def_height_;

                    if (!albedo_rel.empty() && albedo_rel != "-") {
                        m.albedo = LoadSRGBTex(albedo_rel);
                        if (!m.albedo)
                            std::fprintf(stderr,
                                "[terrain][matload] FAILED albedo id=%s path=%s (resolved=%s) -- "
                                "falling back to solid placeholder\n",
                                first.c_str(), albedo_rel.c_str(),
                                fs::absolute(albedo_rel).string().c_str());
                    }
                    if (!normal_rel.empty() && normal_rel != "-") {
                        m.normal = LoadLinearTex(normal_rel);
                        if (!m.normal) {
                            std::fprintf(stderr,
                                "[terrain][matload] FAILED normal id=%s path=%s (resolved=%s) -- "
                                "falling back to default normal\n",
                                first.c_str(), normal_rel.c_str(),
                                fs::absolute(normal_rel).string().c_str());
                            m.normal = def_normal_;
                        }
                    }
                    if (!orm_rel.empty() && orm_rel != "-") {
                        GLuint orm = LoadLinearTex(orm_rel);
                        if (!orm) {
                            std::fprintf(stderr,
                                "[terrain][matload] FAILED orm id=%s path=%s (resolved=%s) -- "
                                "falling back to default roughness/ao\n",
                                first.c_str(), orm_rel.c_str(),
                                fs::absolute(orm_rel).string().c_str());
                        } else {
                            m.roughness = orm;
                            m.ao = orm;  // same ORM: R=AO, G=Roughness, B=Metallic
                        }
                    }

                    if (!m.albedo) {
                        std::fprintf(stderr,
                            "[terrain][matload] material id=%s has NO usable albedo -- "
                            "rendering as solid gray placeholder (200,200,200)\n",
                            first.c_str());
                        m.albedo = MakeSolidTex(200, 200, 200);
                    }
                    materials_.push_back(m);
                    continue;
                }

                // Old format: resolve by folder under data/terrain/materials/<name>/
                std::string mat_dir = "data/terrain/materials/" + first;
                if (!fs::exists(mat_dir) || !fs::is_directory(mat_dir)) {
                    std::fprintf(stderr, "[terrain] material dir not found: %s\n", mat_dir.c_str());
                    // Push placeholder so slot indices match
                    MatTex m;
                    m.albedo    = MakeSolidTex(200, 200, 200);
                    m.normal    = def_normal_;
                    m.roughness = def_roughness_;
                    materials_.push_back(m);
                    continue;
                }

                MatTex m;
                m.tiling    = tiling;
                m.normal    = def_normal_;
                m.roughness = def_roughness_;
                m.ao        = def_ao_;
                m.height    = def_height_;

                for (auto& entry : fs::directory_iterator(mat_dir)) {
                    if (!entry.is_regular_file() || !IsImg(entry.path())) continue;
                    TexRole role = GuessRole(entry.path().stem().string());
                    switch (role) {
                    case TexRole::Albedo:
                        if (!m.albedo) m.albedo = LoadSRGBTex(entry.path().string());
                        break;
                    case TexRole::Normal:
                        if (m.normal == def_normal_)
                            m.normal = LoadLinearTex(entry.path().string());
                        break;
                    case TexRole::Roughness:
                    case TexRole::ORM:
                        if (m.roughness == def_roughness_)
                            m.roughness = LoadLinearTex(entry.path().string());
                        break;
                    case TexRole::AO:
                        if (m.ao == def_ao_)
                            m.ao = LoadLinearTex(entry.path().string());
                        break;
                    case TexRole::Height:
                        if (m.height == def_height_)
                            m.height = LoadLinearTex(entry.path().string());
                        break;
                    default: break;
                    }
                }

                if (!m.albedo) m.albedo = MakeSolidTex(200, 200, 200);
                materials_.push_back(m);
            }
            has_materials_ = !materials_.empty();
        }
    }

    // N-material path setup — only needed once there are actually more than
    // 4 materials; <=4 keeps using splatmap_tex_/materials_[0..3] directly
    // (legacy path in Submit(), untouched). See docs/TECH_DEBT.md "Terrain
    // multi-material authoring (Phase 1)".
    bool usedExtPath = materials_.size() > 4;
    if (usedExtPath) {
        if (splatmap_w_ > 0 && splatmap_h_ > 0 && !splatmap_layers_.empty())
            splatmap_array_.Build(splatmap_w_, splatmap_h_, splatmap_layers_);
        RebuildMaterialArrays();
    } else {
        splatmap_array_.Destroy();
        mat_albedo_array_.Destroy();
        mat_normal_array_.Destroy();
        mat_roughness_array_.Destroy();
        mat_ao_array_.Destroy();
        mat_height_array_.Destroy();
    }

    // Single, one-shot diagnostic summary for the N-material path — see
    // docs/TECH_DEBT.md "Terrain multi-material authoring (Phase 1)". Not
    // per-frame: LoadFromEditor only runs once per area load.
    {
        char magicStr[5] = {
            (char)(dbg_smap_magic & 0xFF), (char)((dbg_smap_magic >> 8) & 0xFF),
            (char)((dbg_smap_magic >> 16) & 0xFF), (char)((dbg_smap_magic >> 24) & 0xFF), 0
        };
        std::fprintf(stderr,
            "[terrain][diag] area='%s' materials_path=%s splatmap_path=%s "
            "materials_parsed=%d splatmap_magic='%s' splatmap_layers=%d "
            "branch=%s num_materials_sent=%d\n",
            area_name.c_str(),
            fs::absolute(mats_path).string().c_str(),
            fs::absolute(smap_path).string().c_str(),
            (int)materials_.size(),
            magicStr, dbg_smap_layers,
            usedExtPath ? "ext(N-materials)" : "legacy(<=4)",
            usedExtPath ? (int)materials_.size() : 0);
    }

    return true;
}

// ---------------------------------------------------------------------------
// RebuildMaterialArrays — builds the 5 N-material texture arrays (albedo/
// normal/roughness/ao/height) from the already-loaded 2D GL textures in
// materials_[i]. Mirrors EditableTerrain::RebuildMaterialArrays (GUE) byte
// for byte — same MaterialTextureArray type, shared via
// rco/renderer/material_texture_array.h. See docs/TECH_DEBT.md "Terrain
// multi-material authoring (Phase 1)".
// ---------------------------------------------------------------------------
void Terrain::RebuildMaterialArrays() {
    int n = (int)materials_.size();
    if (n == 0) return;

    mat_albedo_array_.Resize(n, /*srgb=*/true);
    mat_normal_array_.Resize(n, false);
    mat_roughness_array_.Resize(n, false);
    mat_ao_array_.Resize(n, false);
    mat_height_array_.Resize(n, false);

    for (int i = 0; i < n; ++i) {
        GLuint normal    = materials_[i].normal    ? materials_[i].normal    : def_normal_;
        GLuint roughness = materials_[i].roughness ? materials_[i].roughness : def_roughness_;
        GLuint ao        = materials_[i].ao        ? materials_[i].ao        : def_ao_;
        GLuint height     = materials_[i].height    ? materials_[i].height    : def_height_;
        mat_albedo_array_.SetLayerFromGLTexture(i, materials_[i].albedo);
        mat_normal_array_.SetLayerFromGLTexture(i, normal);
        mat_roughness_array_.SetLayerFromGLTexture(i, roughness);
        mat_ao_array_.SetLayerFromGLTexture(i, ao);
        mat_height_array_.SetLayerFromGLTexture(i, height);
    }

    mat_albedo_array_.GenerateMipmaps();
    mat_normal_array_.GenerateMipmaps();
    mat_roughness_array_.GenerateMipmaps();
    mat_ao_array_.GenerateMipmaps();
    mat_height_array_.GenerateMipmaps();
}

// ---------------------------------------------------------------------------
// RebuildChunksFromHmap
// ---------------------------------------------------------------------------
void Terrain::RebuildChunksFromHmap() {
    chunks_.clear();
    chunks_.reserve(grid_w_ * grid_h_);

    for (int cz = 0; cz < grid_h_; ++cz) {
        for (int cx = 0; cx < grid_w_; ++cx) {
            auto ch = std::make_unique<TerrainChunk>();
            float wx = hmap_origin_x_ + cx * kChunkSize;
            float wz = hmap_origin_z_ + cz * kChunkSize;
            ch->Init(wx, wz, hmap_cell_);

            // Sample from loaded heightmap.
            // stride = kSize-1 so adjacent chunks share their border vertex,
            // eliminating the one-cell gap that caused visible seams.
            // Heights now come from the GPU texture — no per-chunk data needed.
            chunks_.push_back(std::move(ch));
        }
    }
}

// ---------------------------------------------------------------------------
// GenerateProcedural
// ---------------------------------------------------------------------------
void Terrain::GenerateProcedural() {
    chunks_.clear();

    float ox = -(grid_w_ * kChunkSize) * 0.5f;
    float oz = -(grid_h_ * kChunkSize) * 0.5f;

    // Build hmap_data_ so the heightmap GPU texture covers the full terrain.
    const int stride = TerrainChunk::kSize - 1;
    hmap_w_       = grid_w_ * stride + 1;
    hmap_h_       = grid_h_ * stride + 1;
    hmap_cell_    = kCellSize;
    hmap_origin_x_ = ox;
    hmap_origin_z_ = oz;
    hmap_size_x_   = (hmap_w_ - 1) * hmap_cell_;
    hmap_size_z_   = (hmap_h_ - 1) * hmap_cell_;
    has_hmap_      = true;

    hmap_data_.resize(hmap_w_ * hmap_h_);
    for (int z = 0; z < hmap_h_; ++z)
        for (int x = 0; x < hmap_w_; ++x)
            hmap_data_[z * hmap_w_ + x] = ProceduralHeight(
                hmap_origin_x_ + x * hmap_cell_,
                hmap_origin_z_ + z * hmap_cell_);

    if (!hmap_tex_) glGenTextures(1, &hmap_tex_);
    glBindTexture(GL_TEXTURE_2D, hmap_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, hmap_w_, hmap_h_, 0,
                 GL_RED, GL_FLOAT, hmap_data_.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    chunks_.reserve(grid_w_ * grid_h_);
    for (int cz = 0; cz < grid_h_; ++cz) {
        for (int cx = 0; cx < grid_w_; ++cx) {
            auto ch = std::make_unique<TerrainChunk>();
            float wx = ox + cx * kChunkSize;
            float wz = oz + cz * kChunkSize;
            ch->Init(wx, wz, kCellSize);
            chunks_.push_back(std::move(ch));
        }
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void Terrain::SetRenderTuning(const TerrainRenderTuning& tuning) {
    render_tuning_.tiling_mul = glm::clamp(tuning.tiling_mul, 0.50f, 2.50f);
    render_tuning_.macro_strength_mul = glm::clamp(tuning.macro_strength_mul, 0.00f, 3.00f);
    render_tuning_.height_blend_slop = glm::clamp(tuning.height_blend_slop, 0.02f, 0.70f);
}

void Terrain::Submit(Pipeline& pipeline, const glm::vec3& cam_pos) const {
    TerrainChunkSubmission base{};

    // Phase 1: >4 materials uses the generalized N-material shader path
    // (terrainGBufferExt.fs) via the ext_* fields — exactly mirroring
    // EditableTerrain::SubmitToPipeline (GUE). <=4 keeps the legacy
    // exact-4-slot path (num_materials stays 0, default), unchanged from
    // before this session — no regression for the common case. See
    // docs/TECH_DEBT.md "Terrain multi-material authoring (Phase 1)".
    if (materials_.size() > 4) {
        base.num_materials           = (int)materials_.size();
        base.ext_splatmap_array      = splatmap_array_.tex;
        base.ext_num_splat_groups    = splatmap_array_.numGroups;
        base.ext_mat_albedo_array    = mat_albedo_array_.tex;
        base.ext_mat_normal_array    = mat_normal_array_.tex;
        base.ext_mat_roughness_array = mat_roughness_array_.tex;
        base.ext_mat_ao_array        = mat_ao_array_.tex;
        base.ext_mat_height_array    = mat_height_array_.tex;
        base.ext_tilings.reserve(materials_.size());
        base.ext_mat_normal_strength.reserve(materials_.size());
        for (const auto& m : materials_) {
            base.ext_tilings.push_back(m.tiling * render_tuning_.tiling_mul);
            base.ext_mat_normal_strength.push_back(m.normal_strength);
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            if (i < (int)materials_.size()) {
                base.mat_albedo[i]          = materials_[i].albedo;
                base.mat_normal[i]          = materials_[i].normal    ? materials_[i].normal    : def_normal_;
                base.mat_roughness[i]       = materials_[i].roughness ? materials_[i].roughness : def_roughness_;
                base.mat_ao[i]              = materials_[i].ao        ? materials_[i].ao        : def_ao_;
                base.mat_height[i]          = materials_[i].height    ? materials_[i].height    : def_height_;
                base.mat_normal_strength[i] = materials_[i].normal_strength;
            } else {
                base.mat_albedo[i]          = 0;
                base.mat_normal[i]          = def_normal_;
                base.mat_roughness[i]       = def_roughness_;
                base.mat_ao[i]              = def_ao_;
                base.mat_height[i]          = def_height_;
                base.mat_normal_strength[i] = 2.5f;
            }
        }
        base.tilings        = glm::vec4(
            materials_.size() > 0 ? materials_[0].tiling : 4.0f,
            materials_.size() > 1 ? materials_[1].tiling : 4.0f,
            materials_.size() > 2 ? materials_[2].tiling : 4.0f,
            materials_.size() > 3 ? materials_[3].tiling : 4.0f) * render_tuning_.tiling_mul;
    }

    base.macro_variation = macro_tex_ ? macro_tex_ : def_macro_;
    base.macro_strength  = glm::clamp(
        macro_strength_ * render_tuning_.macro_strength_mul,
        0.0f, 1.0f);
    base.height_blend_slop = render_tuning_.height_blend_slop;
    base.splatmap        = splatmap_tex_;
    base.terrain_origin  = { hmap_origin_x_, hmap_origin_z_ };
    base.terrain_size    = { hmap_size_x_,   hmap_size_z_   };
    base.heightmap_tex   = hmap_tex_;
    base.cell_size       = hmap_cell_;

    constexpr float kLodBase = 128.f;  // distance at which LOD 0→1 transition begins

    for (const auto& ch : chunks_) {
        TerrainChunkSubmission c = base;
        c.vao         = ch->vao();
        c.vbo         = ch->vbo();
        c.ebo         = ch->ebo();
        c.index_count = ch->idx_count();
        c.model       = glm::mat4(1.0f);

        // Fractional LOD based on XZ distance from camera to chunk centre
        glm::vec3 origin = ch->Origin();
        float cx   = origin.x + kChunkSize * 0.5f;
        float cz   = origin.z + kChunkSize * 0.5f;
        float dist = glm::length(glm::vec2(cx - cam_pos.x, cz - cam_pos.z));
        c.lod_level = glm::clamp(std::log2(std::max(dist / kLodBase, 1.f)), 0.f, 3.f);

        pipeline.SubmitTerrainChunk(c);
    }
}

// ---------------------------------------------------------------------------
// SampleHeight
// ---------------------------------------------------------------------------
float Terrain::SampleHeight(float wx, float wz) const {
    if (!has_hmap_) return ProceduralHeight(wx, wz);

    // Map world position to heightmap grid coords
    float lx = (wx - hmap_origin_x_) / hmap_cell_;
    float lz = (wz - hmap_origin_z_) / hmap_cell_;

    int x0 = static_cast<int>(std::floor(lx));
    int z0 = static_cast<int>(std::floor(lz));
    float fx = lx - x0;
    float fz = lz - z0;

    auto safe = [&](int x, int z) -> float {
        if (x < 0) x = 0; if (x >= hmap_w_) x = hmap_w_ - 1;
        if (z < 0) z = 0; if (z >= hmap_h_) z = hmap_h_ - 1;
        return hmap_data_[z * hmap_w_ + x];
    };

    float h00 = safe(x0, z0);
    float h10 = safe(x0 + 1, z0);
    float h01 = safe(x0, z0 + 1);
    float h11 = safe(x0 + 1, z0 + 1);

    return h00 * (1 - fx) * (1 - fz)
         + h10 * fx       * (1 - fz)
         + h01 * (1 - fx) * fz
         + h11 * fx       * fz;
}

// ---------------------------------------------------------------------------
// SampleNormal / SlopeAngle
// ---------------------------------------------------------------------------
glm::vec3 Terrain::SampleNormal(float wx, float wz) const {
    const float eps = (hmap_cell_ > 0.f ? hmap_cell_ : 2.f);
    float hr = SampleHeight(wx + eps, wz);
    float hl = SampleHeight(wx - eps, wz);
    float hu = SampleHeight(wx, wz + eps);
    float hd = SampleHeight(wx, wz - eps);
    // Central-difference gradient → upward-pointing surface normal
    return glm::normalize(glm::vec3(-(hr - hl), 2.f * eps, -(hu - hd)));
}

float Terrain::SlopeAngle(float wx, float wz) const {
    return glm::degrees(std::acos(glm::clamp(SampleNormal(wx, wz).y, 0.f, 1.f)));
}

// ---------------------------------------------------------------------------
// UnloadMaterials
// ---------------------------------------------------------------------------
void Terrain::UnloadMaterials() {
    for (auto& m : materials_) {
        auto del = [](GLuint t){ if (t) glDeleteTextures(1, &t); };
        del(m.albedo);
        if (m.normal    != def_normal_)    del(m.normal);
        if (m.roughness != def_roughness_) del(m.roughness);
        if (m.ao        != def_ao_)        del(m.ao);
        if (m.height    != def_height_)    del(m.height);
    }
    materials_.clear();
    has_materials_ = false;
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------
void Terrain::Destroy() {
    chunks_.clear();
    UnloadMaterials();
    if (def_normal_)    { glDeleteTextures(1, &def_normal_);    def_normal_    = 0; }
    if (def_roughness_) { glDeleteTextures(1, &def_roughness_); def_roughness_ = 0; }
    if (def_ao_)        { glDeleteTextures(1, &def_ao_);        def_ao_        = 0; }
    if (def_height_)    { glDeleteTextures(1, &def_height_);    def_height_    = 0; }
    if (def_macro_)     { glDeleteTextures(1, &def_macro_);     def_macro_     = 0; }
    if (macro_tex_)     { glDeleteTextures(1, &macro_tex_);     macro_tex_     = 0; }
    if (splatmap_tex_)  { glDeleteTextures(1, &splatmap_tex_);  splatmap_tex_  = 0; }
    if (hmap_tex_)      { glDeleteTextures(1, &hmap_tex_);      hmap_tex_      = 0; }
    mat_albedo_array_.Destroy();
    mat_normal_array_.Destroy();
    mat_roughness_array_.Destroy();
    mat_ao_array_.Destroy();
    mat_height_array_.Destroy();
    splatmap_array_.Destroy();
    splatmap_layers_.clear();
}

} // namespace rco::renderer
